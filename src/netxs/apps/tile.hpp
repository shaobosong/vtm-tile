// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#pragma once

namespace netxs::events::userland
{
    namespace tile
    {
        struct app_request
        {
            input::hids& gear;
            si32 dir;
        };

        struct app_state
        {
            text label;
            text id;
        };

        EVENTPACK( tile::events, ui::e2::extra::slot4 )
        {
            EVENT_XS( enlist, ui::sptr           ),
            EVENT_XS( delist, bool               ),
            GROUP_XS( ui    , input::hids        ), // Window manager command pack.

            SUBSET_XS( ui )
            {
                EVENT_XS( create  , input::hids ), // Run app if pane is empty.
                EVENT_XS( selectapp, app_request ), // Select default app type for new panes.
                EVENT_XS( setapp,    text        ), // Set the selected default app by id.
                EVENT_XS( selected_app, app_state* ), // Get selected app info.
                EVENT_XS( close    , input::hids ), // Close panes.
                EVENT_XS( closeslot, input::hids ), // Close pane and slot.
                EVENT_XS( rerun   , input::hids ), // Close panes and immediately run the selected app.
                EVENT_XS( swap    , input::hids ), // Swap panes.
                EVENT_XS( rotate  , input::hids ), // Change split orientation.
                EVENT_XS( equalize, input::hids ), // Make panes the same size.
                EVENT_XS( select  , input::hids ), // Focusize all panes.
                EVENT_XS( title   , input::hids ), // Set window manager title using clipboard.
                EVENT_XS( zoom    , input::hids ), // Zoom focused pane.
                GROUP_XS( focus   , input::hids ), // Focusize prev/next pane.
                GROUP_XS( split   , input::hids ), // Split panes.
                GROUP_XS( grips   , twod        ), // Splitting grip modification.

                SUBSET_XS( focus )
                {
                    EVENT_XS( next    , input::hids ),
                    EVENT_XS( prev    , input::hids ),
                    EVENT_XS( lastpane, input::hids ),
                    EVENT_XS( nextpane, input::hids ),
                    EVENT_XS( prevpane, input::hids ),
                    EVENT_XS( nextgrip, input::hids ),
                    EVENT_XS( prevgrip, input::hids ),
                    EVENT_XS( leftpane , input::hids ),
                    EVENT_XS( rightpane, input::hids ),
                    EVENT_XS( uppane   , input::hids ),
                    EVENT_XS( downpane , input::hids ),
                    EVENT_XS( paneindex, input::hids ),
                    EVENT_XS( commandbar, input::hids ),
                    EVENT_XS( pickapp,    input::hids ),
                };
                SUBSET_XS( split )
                {
                    EVENT_XS( vt, input::hids ),
                    EVENT_XS( hz, input::hids ),
                };
                SUBSET_XS( grips )
                {
                    EVENT_XS( move  , twod ),
                    EVENT_XS( resize, si32 ),
                };
            };
        };
    }
}

// tile: Tiling window manager.
namespace netxs::app::tile
{
    static constexpr auto id = "tile";
    static constexpr auto name = "Tiling Window Manager";
    static constexpr auto inheritance_limit = 30; // Tiling limits.

    static auto expand_appcfg(auto& appcfg)
    {
        auto current_module_file = os::process::binary();
        utf::replace_all(appcfg.cmd, "$0", current_module_file);
        utf::replace_all(appcfg.env, "$0", current_module_file);
    }

    static auto set_pane_title(auto& applet, text const& title)
    {
        if (title.empty()) return;
        applet->base::property("applet.header") = title;
        applet->LISTEN(tier::preview, e2::form::prop::ui::header, new_title, -, (fixed_title = title))
        {
            new_title = fixed_title;
        };
    }

    // Directional 2D pane-navigation scoring (shared by the in-tile `navigate`
    // lambda and the workspace-popup `pane_navigate` lambda).
    //
    // Given a source rectangle `src` and a candidate rectangle `dst`, decide whether
    // `dst` lies in the requested direction `dir` (a unit vector along one axis) and
    // produce a score tuple ordered so that `operator<` selects the visually best
    // neighbour. Returns std::nullopt when `dst` is not a valid target.
    //
    // Score tuple (lower is better):
    //   1. gap_dist    - gap along the primary axis; nearest pane wins first.
    //                    Prevents a far pane with full perpendicular span from
    //                    shadowing a near pane with only partial span (e.g. moving
    //                    right out of a tall pane past a stack of shorter panes
    //                    onto a full-height pane further away).
    //   2. center_dist - perpendicular centerline distance; picks the pane best
    //                    visually aligned with the source when several equidistant
    //                    candidates share a border with it.
    //   3. -overlap    - larger perpendicular overlap breaks any remaining ties.
    //
    // Candidates with no perpendicular overlap (corner-only / diagonal neighbours)
    // are excluded: directional navigation should never cross a diagonal gap.
    static auto score_pane_direction(rect src, rect dst, twod dir)
        -> std::optional<std::tuple<si64, si64, si64>>
    {
        auto overlap_1d = [](si64 a1, si64 a2, si64 b1, si64 b2) -> si64
        {
            return std::max<si64>(0, std::min(a2, b2) - std::max(a1, b1));
        };
        auto horizontal = (dir.x != 0);
        auto primary_dir = (si64)(horizontal ? dir.x : dir.y);
        auto gap_dist = si64{};
        auto overlap = si64{};
        auto src_perp_center = si64{}; // Doubled centers (sum of edges) to avoid
        auto dst_perp_center = si64{}; // integer-halving bias.
        if (horizontal)
        {
            auto src_left = (si64)src.coor.x;
            auto src_right = src_left + src.size.x;
            auto dst_left = (si64)dst.coor.x;
            auto dst_right = dst_left + dst.size.x;
            if (primary_dir < 0 && dst_right <= src_left)      gap_dist = src_left - dst_right;
            else if (primary_dir > 0 && dst_left >= src_right) gap_dist = dst_left - src_right;
            else return std::nullopt;
            auto src_top = (si64)src.coor.y;
            auto src_bottom = src_top + src.size.y;
            auto dst_top = (si64)dst.coor.y;
            auto dst_bottom = dst_top + dst.size.y;
            overlap = overlap_1d(src_top, src_bottom, dst_top, dst_bottom);
            src_perp_center = src_top + src_bottom;
            dst_perp_center = dst_top + dst_bottom;
        }
        else
        {
            auto src_top = (si64)src.coor.y;
            auto src_bottom = src_top + src.size.y;
            auto dst_top = (si64)dst.coor.y;
            auto dst_bottom = dst_top + dst.size.y;
            if (primary_dir < 0 && dst_bottom <= src_top)      gap_dist = src_top - dst_bottom;
            else if (primary_dir > 0 && dst_top >= src_bottom) gap_dist = dst_top - src_bottom;
            else return std::nullopt;
            auto src_left = (si64)src.coor.x;
            auto src_right = src_left + src.size.x;
            auto dst_left = (si64)dst.coor.x;
            auto dst_right = dst_left + dst.size.x;
            overlap = overlap_1d(src_left, src_right, dst_left, dst_right);
            src_perp_center = src_left + src_right;
            dst_perp_center = dst_left + dst_right;
        }
        if (overlap <= 0) return std::nullopt;
        auto center_dist = std::abs(src_perp_center - dst_perp_center);
        return std::make_tuple(gap_dist, center_dist, -overlap);
    }

    struct focus_history_t
    {
        std::unordered_map<id_t, ui::wptr> current;
        std::unordered_map<id_t, ui::wptr> previous;
        std::unordered_map<void const*, ui::wptr> owners;

        void remember(id_t gear_id, ui::sptr const& slot_ptr)
        {
            if (!slot_ptr) return;

            auto& current_slot = current[gear_id];
            auto active_slot = current_slot.lock();
            if (active_slot == slot_ptr) return;

            if (active_slot) previous[gear_id] = active_slot;
            current_slot = slot_ptr;
        }

        void replace(ui::sptr const& old_slot_ptr, ui::sptr const& new_slot_ptr)
        {
            if (!old_slot_ptr) return;

            auto replace_slot = [&](auto& slots)
            {
                for (auto& [gear_id, slot_wptr] : slots)
                {
                    if (slot_wptr.lock() == old_slot_ptr)
                    {
                        slot_wptr = new_slot_ptr;
                    }
                }
            };
            replace_slot(current);
            replace_slot(previous);
        }

        void bind(ui::sptr const& focus_target_ptr, ui::sptr const& slot_ptr)
        {
            if (!focus_target_ptr) return;
            owners[focus_target_ptr.get()] = slot_ptr;
        }

        auto owner(ui::sptr const& focus_target_ptr)
        {
            if (!focus_target_ptr) return ui::sptr{};
            if (auto iter = owners.find(focus_target_ptr.get()); iter != owners.end())
            {
                if (auto slot_ptr = iter->second.lock())
                {
                    return slot_ptr;
                }
                owners.erase(iter);
            }
            return ui::sptr{};
        }

        auto last(id_t gear_id)
        {
            if (auto iter = previous.find(gear_id); iter != previous.end())
            {
                if (auto slot_ptr = iter->second.lock())
                {
                    return slot_ptr;
                }
                previous.erase(iter);
            }
            return ui::sptr{};
        }
    };

    static auto track_slot_focus(auto focus_target_ptr, auto slot_ptr, auto focus_history_ptr)
    {
        if (focus_history_ptr) focus_history_ptr->bind(focus_target_ptr, slot_ptr);

        auto target_shadow = ptr::shadow(focus_target_ptr);
        focus_target_ptr->LISTEN(tier::release, e2::form::state::focus::on, gear_id, -, (focus_history_ptr, target_shadow))
        {
            if (focus_history_ptr)
            if (auto target_ptr = target_shadow.lock())
            if (auto slot_ptr = focus_history_ptr->owner(target_ptr))
            {
                focus_history_ptr->remember(gear_id, slot_ptr);
            }
        };
        return focus_target_ptr;
    }

    static auto get_slot_focus_target(ui::sptr const& slot_ptr)
    {
        if (!slot_ptr) return ui::sptr{};

        auto node_veer_ptr = std::dynamic_pointer_cast<ui::veer>(slot_ptr);
        if (!node_veer_ptr || !node_veer_ptr->count()) return ui::sptr{};

        auto item_ptr = node_veer_ptr->back();
        if (!item_ptr) return ui::sptr{};

        if (node_veer_ptr->count() == 1) return item_ptr; // Empty slot.

        if (item_ptr->root())
        {
            if (auto applet_host_ptr = std::dynamic_pointer_cast<ui::fork>(item_ptr))
            {
                if (auto applet_ptr = applet_host_ptr->get(slot::_2))
                {
                    return applet_ptr;
                }
            }
        }
        return item_ptr;
    }

    // Property name on the tile boss that stores a resolver mapping a gear id
    // to the currently focused applet inside the active workspace. Installed
    // by the tile boss during construction; consumed by the vtm.terminal Lua
    // proxy (see install_terminal_proxy below) so that scripts invoked from
    // anywhere (keybinds, command bar, prerun) can be forwarded to the right
    // dtvt child process at call time.
    static constexpr auto terminal_proxy_resolver_field = "tile.terminal_proxy_resolver";
    using terminal_proxy_resolver_t = std::function<ui::sptr(netxs::id_t /*gear_id*/)>;

    // Broadcaster for vtm.terminal.* scripts. Callers (the proxy below) pass a
    // gear id and a visitor; the broadcaster invokes the visitor once per
    // applet that is currently focused for that gear (i.e., every pane that
    // SelectAllPanes / multi-focus has marked as "selected"). Installed on the
    // tile boss in parallel with terminal_proxy_resolver_field so that scripts
    // dispatched from the command bar / keybinds / prerun fan out to all
    // selected panes instead of only the last-remembered one. When no applet
    // is currently focused for the gear, the broadcaster invokes the visitor
    // zero times and the proxy falls back to the single-target resolver to
    // preserve existing single-focus semantics.
    static constexpr auto terminal_proxy_broadcaster_field = "tile.terminal_proxy_broadcaster";
    using terminal_proxy_broadcaster_visitor_t = std::function<void(ui::sptr& /*applet_ptr*/)>;
    using terminal_proxy_broadcaster_t = std::function<void(netxs::id_t /*gear_id*/, terminal_proxy_broadcaster_visitor_t const& /*visit*/)>;

    // Lua C function: __index on the vtm.terminal proxy table installed in the
    // tile manager's lua_State. Receives (proxy_table, method_name) on stack
    // and returns a closure carrying the method name as upvalue. Calling that
    // closure invokes tile_terminal_proxy_call below, which rebuilds the
    // script string "vtm.terminal.<method>(<args>)" and signals it to the
    // currently focused dtvt pane via e2::command::run; the dtvt parent-side
    // listener forwards over the pipe and the child re-fires + runs it in its
    // own Lua engine where vtm.terminal.* is actually bound.
    static netxs::si32 tile_terminal_proxy_call(::lua_State* lua);
    static netxs::si32 tile_terminal_proxy_index(::lua_State* lua)
    {
        // Stack: 1=proxy_table, 2=method_name(string).
        ::lua_pushvalue(lua, 2);                                       // Dup method name as upvalue.
        ::lua_pushcclosure(lua, &tile_terminal_proxy_call, 1);
        return 1;
    }

    // Quote a Lua-side argument back into Lua source form. Strings get a
    // backslash-escaped double-quoted form; numbers and booleans pass through
    // verbatim; other types degrade to nil to keep the regenerated script
    // syntactically valid (the receiving terminal will simply ignore the arg).
    static auto tile_terminal_proxy_quote_arg(::lua_State* lua, netxs::si32 idx)
    {
        auto crop = netxs::text{};
        auto type = ::lua_type(lua, idx);
        if (type == LUA_TBOOLEAN)
        {
            crop = ::lua_toboolean(lua, idx) ? "true" : "false";
        }
        else if (type == LUA_TNUMBER)
        {
            ::lua_pushvalue(lua, idx);
            auto len = size_t{};
            auto ptr = ::lua_tolstring(lua, -1, &len);
            crop.assign(ptr, len);
            ::lua_pop(lua, 1);
        }
        else if (type == LUA_TSTRING)
        {
            auto len = size_t{};
            auto ptr = ::lua_tolstring(lua, idx, &len);
            crop.reserve(len + 2);
            crop.push_back('"');
            for (size_t i = 0; i < len; i++)
            {
                auto c = ptr[i];
                switch (c)
                {
                    case '\\': crop += "\\\\"; break;
                    case '"':  crop += "\\\""; break;
                    case '\n': crop += "\\n";  break;
                    case '\r': crop += "\\r";  break;
                    case '\t': crop += "\\t";  break;
                    default:
                        if ((unsigned char)c < 0x20) // Other control chars: \xHH-style escape.
                        {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\%d", (int)(unsigned char)c);
                            crop += buf;
                        }
                        else crop.push_back(c);
                        break;
                }
            }
            crop.push_back('"');
        }
        else
        {
            // nil / table / function / userdata / unsupported: degrade to nil.
            crop = "nil";
        }
        return crop;
    }

    static netxs::si32 tile_terminal_proxy_call(::lua_State* lua)
    {
        // Stack: 1..N = forwarded method args.
        // Upvalue 1: method name (string).
        return netxs::events::luna::vtmlua_run_with_indexer(lua, [&](netxs::events::auth& indexer)
        {
            auto fx_name_ptr = ::lua_tostring(lua, lua_upvalueindex(1));
            if (!fx_name_ptr) return 0;
            auto fx_name = view{ fx_name_ptr };

            // Find the tile boss (single instance per process); without it we
            // have nowhere to dispatch and would emit a noisy
            // "vtm.terminal.X" against a non-existent target.
            auto class_iter = indexer.classes.find(basename::tile);
            if (class_iter == indexer.classes.end() || !class_iter->second) return 0;
            auto& subobjects = class_iter->second->objects;
            if (subobjects.empty()) return 0;
            auto& boss = subobjects.front().get();

            // Resolve target applet(s) for the active gear. Active gear is
            // set by callers before script execution (luafx.set_gear in
            // command bar; bindings::keybind path in input::bindings).
            //
            // Dispatch is strictly broadcaster-only: fan the script out to
            // every currently focused applet for this gear via
            // foreach(gear.id, ...) — the same walk that vtm.tile.* commands
            // (split, rotate, close, ...) use. If no applet is currently
            // focused (e.g., user dropped focus from every pane via
            // Ctrl+LeftClick), the script is intentionally a no-op rather
            // than being routed to a "last-defocused" pane via focus
            // history. Routing to an unfocused pane was surprising: the
            // user could not see which pane the dispatch landed on, and
            // visual feedback (e.g., the find bar appearing) showed up on
            // a pane the user had explicitly stepped away from.
            auto& gear = indexer.active_gear_ref.get();

            // Rebuild script source: vtm.terminal.<fx>(arg1, arg2, ...).
            auto args_count = ::lua_gettop(lua);
            auto script = text{ "vtm.terminal." } + text{ fx_name } + "(";
            for (auto i = 1; i <= args_count; i++)
            {
                if (i > 1) script += ", ";
                script += tile_terminal_proxy_quote_arg(lua, i);
            }
            script += ")";

            auto cmd = eccc{ .cmd = script };
            auto& broadcaster = boss.base::template property<terminal_proxy_broadcaster_t>(terminal_proxy_broadcaster_field);
            if (broadcaster)
            {
                broadcaster(gear.id, [&](ui::sptr& applet_ptr)
                {
                    if (!applet_ptr) return;
                    applet_ptr->base::signal(tier::release, e2::command::run, cmd);
                });
            }
            return 0;
        });
    }

    static auto is_standalone_tile(ui::base& boss)
    {
        return !boss.base::signal(tier::general, e2::config::creator);
    }

    static void close_tile_session(ui::base& boss, view reason)
    {
        if (is_standalone_tile(boss))
        {
            boss.base::signal(tier::general, e2::shutdown, utf::concat(prompt::tile, reason));
        }
        else
        {
            boss.base::riseup(tier::release, e2::form::proceed::quit::one, true);
        }
    }

    struct apps_data_t
    {
        text selected_id;
        text selected_label;
        std::vector<text> ids;
        std::vector<text> labels;
        std::vector<text> cmds;
        size_t selected_index = std::numeric_limits<size_t>::max();
    };

    static auto get_apps_data(ui::base& boss)
    {
        auto& indexer = ui::tui_domain();
        auto& config = indexer.config;
        auto tile_app_context = config.settings::push_context("/config/tile/app");
        auto default_selected_id = config.settings::take("/config/tile/app/selected", "term"s);
        auto& selected_id_property = boss.base::property("tile.selected");
        auto selected_id = selected_id_property.empty() ? default_selected_id : static_cast<text>(selected_id_property);
        auto item_list = config.settings::take_ptr_list_for_name("item");

        auto res = apps_data_t{};
        res.selected_id = selected_id;

        for (auto& item_ptr : item_list)
        {
            auto item_id = config.settings::take_value_from(item_ptr, "id", text{});
            auto item_label = config.settings::take_value_from(item_ptr, "label", item_id);
            auto item_cmd = config.settings::take_value_from(item_ptr, "cmd", text{});
            if (!item_id.empty())
            {
                if (item_id == selected_id)
                {
                    res.selected_index = res.ids.size();
                    res.selected_label = item_label;
                }
                res.ids.push_back(item_id);
                res.labels.push_back(item_label);
                res.cmds.push_back(item_cmd);
            }
        }
        return res;
    }

    namespace events = netxs::events::userland::tile;

    using ui::sptr;
    using ui::wptr;

    struct ws_thumb_pane_t
    {
        rect area;        // Position within thumbnail coordinate space.
        text label;       // Pane label (e.g., app title).
        si32 color_idx;   // Color index (0-3) for adjacency-aware coloring.
        sptr slot_veer;   // Pointer to the node_veer for click handling.
    };

    #define proc_list \
        X(FocusNextPaneOrGrip) \
        X(FocusNextPane      ) \
        X(LastPane           ) \
        X(FocusLeftPane      ) \
        X(FocusRightPane     ) \
        X(FocusUpPane        ) \
        X(FocusDownPane      ) \
        X(FocusNextGrip      ) \
        X(MoveGrip           ) \
        X(ResizeGrip         ) \
        X(RunApplication     ) \
        X(ReRunApplication   ) \
        X(SelectApplication  ) \
        X(SelectedApp        ) \
        X(SetSelectedApp     ) \
        X(SelectAllPanes     ) \
        X(SplitPane          ) \
        X(RotateSplit        ) \
        X(SwapPanes          ) \
        X(EqualizeSplitRatio ) \
        X(SetTitle           ) \
        X(ZoomPane           ) \
        X(ClosePane          ) \
        X(CloseSlot          ) \
        X(SetMenuColor       ) \
        X(ShowPaneIndex      ) \
        X(Disconnect         ) \
        X(Shutdown           ) \
        X(CreateWorkspace    ) \
        X(DestroyWorkspace   ) \
        X(SwitchWorkspace    ) \
        X(NextWorkspace      ) \
        X(PrevWorkspace      ) \
        X(LastWorkspace      ) \
        X(CurrentWorkspace   ) \
        X(OpenWorkspacePopup ) \
        X(OpenCommandBar     ) \
        X(PickApplication    ) \

    struct methods
    {
        #define X(_proc) static constexpr auto _proc = #_proc;
        proc_list
        #undef X
    };

    #undef proc_list

    namespace command_bar
    {
        struct item
        {
            text display;
            text tooltip;
            text script;
        };

        struct model
        {
            std::vector<si32> filtered;
            si32              tooltip_col = 6; // 2-cell indicator prefix + 4-cell gap.
            si32              max_tooltip = 0;
        };

        struct layout
        {
            bool ok = faux;

            si32 full_w = 0;
            si32 full_h = 0;
            si32 dlg_w = 0;
            si32 dlg_h = 0;
            si32 dlg_x = 0;
            si32 dlg_y = 0;
            si32 inner_x = 0;
            si32 vsb_x = 0;

            si32 n_total = 0;
            si32 n_visible = 0;
            si32 list_rows = 0;
            si32 entry_disp_w = 0;
            si32 list_disp_w = 0;
            si32 list_content_w = 0;

            si32 max_vscroll = 0;
            si32 max_hscroll = 0;
            si32 max_list_hscroll = 0;

            si32 v_scroll = 0;
            si32 h_scroll = 0;
            si32 list_h_scroll = 0;
            bool has_hsb = faux;
        };

        static constexpr auto dlg_w_max = si32{ 60 };
        static constexpr auto max_items = si32{ 10 };

        static constexpr auto bg              = 0xFF1E1E2Eu;
        static constexpr auto surface         = 0xFF313244u;
        static constexpr auto border          = 0xFF45475Au;
        static constexpr auto text_fg         = 0xFFCDD6F4u;
        static constexpr auto subtext         = 0xFF6C7086u;
        static constexpr auto sel_bg          = 0xFF89B4FAu;
        static constexpr auto sel_fg          = 0xFF1E1E2Eu;
        static constexpr auto prompt_fg       = 0xFF89DCEBu;
        static constexpr auto scroll_track    = 0xFF2C3047u;
        static constexpr auto scroll_thumb    = 0xFF3B4261u;
        static constexpr auto scroll_hover    = 0xFF565F89u;
        static constexpr auto scroll_drag     = 0xFF89B4FAu;
        static constexpr auto match_fg        = 0xFFF9E2AFu;
        static constexpr auto match_sel_fg    = 0xFFFFFFFFu;

        // Bitmask flags that a caller may set via pending_cmd_flags_ptr before opening
        // the command bar to opt-in to extra keyboard shortcuts for that session.
        // New per-session capabilities should be added here as new bit values.
        enum flags : si32
        {
            none           = 0,
            allow_split    = 1 << 0,  // Ctrl+V (vertical) / Ctrl+S (horizontal) split shortcuts.
            allow_replace  = 1 << 1,  // Ctrl+R (close pane + RunApplication) shortcut.
        };

        static auto utf8_step(view utf8, size_t offset) -> size_t
        {
            auto uc = (unsigned char)utf8[offset];
            auto len = uc < 0x80 ? size_t{ 1 }
                     : (uc & 0xE0) == 0xC0 ? size_t{ 2 }
                     : (uc & 0xF0) == 0xE0 ? size_t{ 3 }
                     : size_t{ 4 };
            return std::min(len, utf8.size() - offset);
        }

        static auto skip_codepoints(view utf8, si32 count) -> size_t
        {
            auto offset = size_t{};
            auto skipped = si32{};
            while (offset < utf8.size() && skipped < count)
            {
                offset += utf8_step(utf8, offset);
                ++skipped;
            }
            return offset;
        }

        static auto byte_of_cp(view utf8, si32 cp) -> size_t
        {
            if (cp <= 0) return 0;
            auto offset = size_t{};
            auto n = si32{};
            while (offset < utf8.size() && n < cp)
            {
                offset += utf8_step(utf8, offset);
                ++n;
            }
            return offset;
        }

        static auto cp_len(view utf8) -> si32
        {
            auto n = si32{};
            for (auto c : utf8)
            {
                n += ((unsigned char)c & 0xC0) != 0x80;
            }
            return n;
        }

        static auto filter_input_text(view utf8) -> text
        {
            auto buf = text{};
            buf.reserve(utf8.size());
            for (auto i = size_t{}; i < utf8.size();)
            {
                auto c = (unsigned char)utf8[i];
                if (c < 0x20 || c == 0x7f)
                {
                    ++i;
                    continue;
                }
                auto n = utf8_step(utf8, i);
                buf.append(utf8.data() + i, n);
                i += n;
            }
            return buf;
        }

        enum class char_class : si32
        {
            white,
            non_word,
            delimiter,
            lower,
            upper,
            letter,
            number,
        };

        struct fuzzy_result
        {
            bool                matched = faux;
            si32                score   = 0;
            std::vector<size_t> offsets;
        };

        static constexpr auto score_match                 = si32{ 16 };
        static constexpr auto score_gap_start             = si32{ -3 };
        static constexpr auto score_gap_extension         = si32{ -1 };
        static constexpr auto bonus_boundary              = score_match / 2;
        static constexpr auto bonus_non_word              = score_match / 2;
        static constexpr auto bonus_camel123              = bonus_boundary + score_gap_extension;
        static constexpr auto bonus_consecutive           = -(score_gap_start + score_gap_extension);
        static constexpr auto bonus_first_char_multiplier = si32{ 2 };
        static constexpr auto bonus_boundary_white        = bonus_boundary + 2;
        static constexpr auto bonus_boundary_delimiter    = bonus_boundary + 1;

        static auto ascii_lower(unsigned char c) -> char
        {
            return (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        }

        static auto is_fuzzy_white(unsigned char c) -> bool
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
        }

        static auto is_fuzzy_delimiter(unsigned char c) -> bool
        {
            return c == '/' || c == ',' || c == ':' || c == ';' || c == '|';
        }

        static auto class_of(unsigned char c) -> char_class
        {
            if (c >= 'a' && c <= 'z') return char_class::lower;
            if (c >= 'A' && c <= 'Z') return char_class::upper;
            if (c >= '0' && c <= '9') return char_class::number;
            if (is_fuzzy_white(c))    return char_class::white;
            if (is_fuzzy_delimiter(c)) return char_class::delimiter;
            return char_class::non_word;
        }

        static auto bonus_for(char_class prev, char_class curr) -> si32
        {
            if ((si32)curr > (si32)char_class::non_word)
            {
                switch (prev)
                {
                    case char_class::white:     return bonus_boundary_white;
                    case char_class::delimiter: return bonus_boundary_delimiter;
                    case char_class::non_word:  return bonus_boundary;
                    default: break;
                }
            }

            if ((prev == char_class::lower && curr == char_class::upper)
             || (prev != char_class::number && curr == char_class::number))
            {
                return bonus_camel123;
            }

            switch (curr)
            {
                case char_class::non_word:
                case char_class::delimiter: return bonus_non_word;
                case char_class::white:     return bonus_boundary_white;
                default:                    return 0;
            }
        }

        static auto max3(si32 a, si32 b, si32 c) -> si32
        {
            return std::max(a, std::max(b, c));
        }

        static auto greedy_offsets(view query, view target) -> std::vector<size_t>
        {
            auto offsets = std::vector<size_t>{};
            auto ti = size_t{};
            for (auto qch : query)
            {
                auto lc = ascii_lower((unsigned char)qch);
                while (ti < target.size() && ascii_lower((unsigned char)target[ti]) != lc) ++ti;
                if (ti >= target.size()) break;
                offsets.push_back(ti);
                ++ti;
            }
            return offsets;
        }

        static auto fuzzy_search(view query, view target) -> fuzzy_result
        {
            auto result = fuzzy_result{};
            if (query.empty())
            {
                result.matched = true;
                return result;
            }

            auto const m = (si32)query.size();
            auto const n = (si32)target.size();
            if (m > n) return result;

            auto pattern = std::vector<char>{};
            pattern.reserve(query.size());
            for (auto c : query)
            {
                pattern.push_back(ascii_lower((unsigned char)c));
            }

            auto text_chars = std::vector<char>((size_t)n);
            auto bonuses    = std::vector<si32>((size_t)n);
            auto first_pos  = std::vector<si32>((size_t)m, -1);

            auto pidx = si32{};
            auto pchr = pattern.front();
            auto last_idx = si32{ -1 };
            auto prev_class = char_class::white;
            for (auto i = si32{}; i < n; ++i)
            {
                auto raw = (unsigned char)target[(size_t)i];
                auto cls = class_of(raw);
                auto chr = ascii_lower(raw);
                text_chars[(size_t)i] = chr;
                bonuses[(size_t)i] = bonus_for(prev_class, cls);
                prev_class = cls;

                if (chr == pchr)
                {
                    if (pidx < m)
                    {
                        first_pos[(size_t)pidx] = i;
                        ++pidx;
                        pchr = pattern[(size_t)std::min(pidx, m - 1)];
                    }
                    last_idx = i;
                }
            }
            if (pidx != m) return result;

            result.matched = true;

            auto h0 = std::vector<si32>((size_t)n);
            auto c0 = std::vector<si32>((size_t)n);
            auto max_score = si32{};
            auto max_score_pos = first_pos.front();
            auto prev_h0 = si32{};
            auto in_gap = faux;
            for (auto i = si32{}; i < n; ++i)
            {
                if (text_chars[(size_t)i] == pattern.front())
                {
                    auto score = score_match + bonuses[(size_t)i] * bonus_first_char_multiplier;
                    h0[(size_t)i] = score;
                    c0[(size_t)i] = 1;
                    if (m == 1 && score > max_score)
                    {
                        max_score = score;
                        max_score_pos = i;
                    }
                    in_gap = faux;
                }
                else
                {
                    h0[(size_t)i] = std::max(prev_h0 + (in_gap ? score_gap_extension : score_gap_start), si32{ 0 });
                    c0[(size_t)i] = 0;
                    in_gap = true;
                }
                prev_h0 = h0[(size_t)i];
            }

            if (m == 1)
            {
                result.score = max_score;
                result.offsets.push_back((size_t)max_score_pos);
                return result;
            }

            auto const first_idx = first_pos.front();
            auto const width = last_idx - first_idx + 1;
            auto h = std::vector<si32>((size_t)(width * m));
            auto c = std::vector<si32>((size_t)(width * m));
            for (auto i = first_idx; i <= last_idx; ++i)
            {
                auto dst = (size_t)(i - first_idx);
                h[dst] = h0[(size_t)i];
                c[dst] = c0[(size_t)i];
            }

            for (auto pattern_idx = si32{ 1 }; pattern_idx < m; ++pattern_idx)
            {
                auto const row = pattern_idx * width;
                auto const prev_row = row - width;
                auto gap = faux;
                for (auto col = first_pos[(size_t)pattern_idx]; col <= last_idx; ++col)
                {
                    auto const j0 = col - first_idx;
                    auto const idx = row + j0;
                    auto const left = j0 > 0 ? h[(size_t)(idx - 1)] : si32{};
                    auto s2 = left + (gap ? score_gap_extension : score_gap_start);
                    auto s1 = si32{};
                    auto consecutive = si32{};

                    if (text_chars[(size_t)col] == pattern[(size_t)pattern_idx])
                    {
                        auto const diag = prev_row + j0 - 1;
                        s1 = h[(size_t)diag] + score_match;
                        auto bonus = bonuses[(size_t)col];
                        consecutive = c[(size_t)diag] + 1;
                        if (consecutive > 1)
                        {
                            auto first_bonus = bonuses[(size_t)(col - consecutive + 1)];
                            if (bonus >= bonus_boundary && bonus > first_bonus)
                            {
                                consecutive = 1;
                            }
                            else
                            {
                                bonus = max3(bonus, bonus_consecutive, first_bonus);
                            }
                        }
                        if (s1 + bonus < s2)
                        {
                            s1 += bonuses[(size_t)col];
                            consecutive = 0;
                        }
                        else
                        {
                            s1 += bonus;
                        }
                    }

                    c[(size_t)idx] = consecutive;
                    gap = s1 < s2;
                    auto score = max3(s1, s2, 0);
                    if (pattern_idx == m - 1 && score > max_score)
                    {
                        max_score = score;
                        max_score_pos = col;
                    }
                    h[(size_t)idx] = score;
                }
            }

            result.score = max_score;
            result.offsets.reserve(query.size());
            auto pattern_idx = m - 1;
            auto col = max_score_pos;
            auto prefer_match = true;
            while (col >= first_idx)
            {
                auto const row = pattern_idx * width;
                auto const j0 = col - first_idx;
                auto const idx = row + j0;
                auto const score = h[(size_t)idx];
                auto s1 = si32{};
                auto s2 = si32{};
                if (pattern_idx > 0 && col >= first_pos[(size_t)pattern_idx] && j0 > 0)
                {
                    s1 = h[(size_t)(idx - width - 1)];
                }
                if (col > first_pos[(size_t)pattern_idx] && j0 > 0)
                {
                    s2 = h[(size_t)(idx - 1)];
                }

                if (score > s1 && (score > s2 || (score == s2 && prefer_match)))
                {
                    result.offsets.push_back((size_t)col);
                    if (pattern_idx == 0) break;
                    --pattern_idx;
                }

                auto const next_idx = idx + width + 1;
                prefer_match = c[(size_t)idx] > 1
                             || (next_idx < (si32)c.size() && c[(size_t)next_idx] > 0);
                --col;
            }

            if ((si32)result.offsets.size() != m)
            {
                result.offsets = greedy_offsets(query, target);
            }
            else
            {
                std::sort(result.offsets.begin(), result.offsets.end());
            }
            return result;
        }

        static auto fuzzy_terms(view query) -> std::vector<view>
        {
            auto terms = std::vector<view>{};
            auto pos = size_t{};
            while (pos < query.size())
            {
                while (pos < query.size() && is_fuzzy_white((unsigned char)query[pos])) ++pos;
                auto start = pos;
                while (pos < query.size() && !is_fuzzy_white((unsigned char)query[pos])) ++pos;
                if (start < pos)
                {
                    terms.push_back(query.substr(start, pos - start));
                }
            }
            return terms;
        }

        static auto fuzzy_search_terms(std::vector<view> const& terms, view target) -> fuzzy_result
        {
            auto result = fuzzy_result{};
            if (terms.empty())
            {
                result.matched = true;
                return result;
            }

            result.matched = true;
            for (auto term : terms)
            {
                auto match = fuzzy_search(term, target);
                if (!match.matched) return {};
                result.score += match.score;
                result.offsets.insert(result.offsets.end(), match.offsets.begin(), match.offsets.end());
            }
            std::sort(result.offsets.begin(), result.offsets.end());
            result.offsets.erase(std::unique(result.offsets.begin(), result.offsets.end()), result.offsets.end());
            return result;
        }

        static auto fuzzy_search_terms(view query, view target) -> fuzzy_result
        {
            return fuzzy_search_terms(fuzzy_terms(query), target);
        }

        [[maybe_unused]] static auto fuzzy_match(view query, view target) -> bool
        {
            return fuzzy_search_terms(query, target).matched;
        }

        static auto match_offsets(view query, view target) -> std::vector<size_t>
        {
            return fuzzy_search_terms(query, target).offsets;
        }

        static auto load(auto& cfg) -> netxs::sptr<std::vector<item>>
        {
            auto commands = ptr::shared(std::vector<item>{});
            auto bar_ctx = cfg.settings::push_context("/config/tile/commandbar");
            auto group_ptr_list = cfg.settings::take_ptr_list_for_name("group");
            for (auto& g_ptr : group_ptr_list)
            {
                auto group_label = cfg.settings::take_value_from(g_ptr, "label", ""s);
                auto item_ptr_list = cfg.settings::take_ptr_list_of(g_ptr, "item");
                for (auto& i_ptr : item_ptr_list)
                {
                    auto label   = cfg.settings::take_value_from(i_ptr, "label",   ""s);
                    auto tooltip = cfg.settings::take_value_from(i_ptr, "tooltip", ""s);
                    auto script  = cfg.settings::take_value_from(i_ptr, "script",  ""s);
                    commands->push_back({ group_label + ": " + label, tooltip, script });
                }
            }
            return commands;
        }

        static auto build_model(std::vector<item> const& commands, view query) -> model
        {
            struct ranked_match
            {
                si32 index;
                si32 score;
                si32 length;
            };

            auto result = model{};
            auto terms = fuzzy_terms(query);
            auto ranked = std::vector<ranked_match>{};
            ranked.reserve(commands.size());
            for (auto i = si32{}; i < (si32)commands.size(); ++i)
            {
                auto match = fuzzy_search_terms(terms, commands[i].display);
                if (!match.matched) continue;
                auto length = (si32)utf::length(commands[i].display);
                if (terms.empty())
                {
                    result.filtered.push_back(i);
                }
                else
                {
                    ranked.push_back({ i, match.score, length });
                }
                result.tooltip_col = std::max(result.tooltip_col, length + 6);
                result.max_tooltip = std::max(result.max_tooltip, (si32)utf::length(commands[i].tooltip));
            }
            std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b)
            {
                if (a.score  != b.score ) return a.score  > b.score;
                if (a.length != b.length) return a.length < b.length;
                return a.index < b.index;
            });
            for (auto const& match : ranked)
            {
                result.filtered.push_back(match.index);
            }
            return result;
        }

        static auto layout_of(twod size, model const& data, si32 query_len,
                              si32 v_scroll, si32 h_scroll, si32 list_h_scroll) -> layout
        {
            auto l = layout{};
            l.full_w = size.x;
            l.full_h = size.y;
            if (l.full_w < 10 || l.full_h < 4) return l;

            l.n_total = (si32)data.filtered.size();
            l.n_visible = (si32)std::min(l.n_total, max_items);
            l.dlg_w = std::min(dlg_w_max, l.full_w - 4);
            if (l.dlg_w < 10) return l;

            l.entry_disp_w = l.dlg_w - 4;
            l.list_disp_w = l.dlg_w - 4;
            l.list_content_w = (data.tooltip_col - 2) + data.max_tooltip;
            l.max_list_hscroll = std::max(0, l.list_content_w - l.list_disp_w);
            l.has_hsb = l.list_content_w > l.list_disp_w;

            l.list_rows = data.filtered.empty() ? 1 : l.n_visible;
            l.dlg_h = 2 + l.list_rows + 1;
            l.dlg_x = (l.full_w - l.dlg_w) / 2;
            l.dlg_y = std::max(0, (l.full_h - (2 + max_items + 1)) / 4);
            l.inner_x = l.dlg_x + 1;
            l.vsb_x = l.dlg_x + l.dlg_w - 1;

            l.max_vscroll = std::max(0, l.n_total - max_items);
            l.max_hscroll = std::max(0, query_len - l.entry_disp_w + 1);
            l.v_scroll = std::clamp(v_scroll, 0, l.max_vscroll);
            l.h_scroll = std::clamp(h_scroll, 0, l.max_hscroll);
            l.list_h_scroll = std::clamp(list_h_scroll, 0, l.max_list_hscroll);
            l.ok = true;
            return l;
        }

        static auto put_text(auto& canvas, auto link, si32 x, si32 y,
                             view utf8, ui32 fg, ui32 bgc, si32 max_cells,
                             si32 skip = 0) -> void
        {
            auto i = skip_codepoints(utf8, skip);
            auto xi = si32{};
            while (i < utf8.size() && xi < max_cells)
            {
                auto seq_len = utf8_step(utf8, i);
                auto ch = utf8.substr(i, seq_len);
                canvas.fill(rect{{ x + xi, y }, { 1, 1 }}, [=](cell& c)
                {
                    c.bgc(bgc).fgc(fg).txt(ch).link(link);
                });
                i += seq_len;
                ++xi;
            }
        }

        static auto put_highlighted(auto& canvas, auto link, si32 x, si32 y,
                                    view utf8, ui32 fg, ui32 match, ui32 bgc,
                                    si32 max_cells, view query, si32 skip = 0) -> void
        {
            auto matches = match_offsets(query, utf8);
            auto i = skip_codepoints(utf8, skip);
            auto match_iter = matches.cbegin();
            while (match_iter != matches.cend() && *match_iter < i) ++match_iter;

            auto xi = si32{};
            while (i < utf8.size() && xi < max_cells)
            {
                auto seq_len = utf8_step(utf8, i);
                auto ch = utf8.substr(i, seq_len);
                auto is_hit = match_iter != matches.cend() && *match_iter == i;
                if (is_hit) ++match_iter;
                auto cur_fg = is_hit ? match : fg;
                canvas.fill(rect{{ x + xi, y }, { 1, 1 }}, [=](cell& c)
                {
                    c.bgc(bgc).fgc(cur_fg).txt(ch).link(link);
                });
                i += seq_len;
                ++xi;
            }
        }
    }

    // tile: Right-side item list.
    class items
        : public pro::skill
    {
        using skill::boss,
              skill::memo;

        netxs::sptr<ui::list>    client;
        si32                     window_state;
        std::unordered_set<id_t> data_sources;

    public:
        items(base&&) = delete;
        items(base& boss)
            : skill{ boss },
              client{ boss.attach(ui::list::ctor(axis::Y)) },// sort::reverse)) },
              window_state{ winstate::undefined }
        {
            boss.LISTEN(tier::release, e2::area, new_area, memo)
            {
                auto coor = twod{ new_area.size.x + 2/*resize grip width*/, 0 };
                client->base::moveto(coor);
            };
            boss.LISTEN(tier::release, tile::events::enlist, data_src_sptr, memo)
            {
                if (!data_src_sptr || data_sources.find(data_src_sptr->id) != data_sources.end()) return; // Deduplicate.
                auto active_color = skin::color(tone::active);
                auto focused_color = skin::color(tone::focused);
                auto cF = focused_color;
                auto cE = active_color;
                auto current_title = data_src_sptr->base::signal(tier::request, e2::form::prop::ui::header);
                static auto label_format = [](view utf8){ return utf8.empty() ? "- no title -"sv : utf8; };
                client->attach(ui::item::ctor(label_format(current_title)))
                    ->setpad({ 1, 1 })
                    ->active(cE)
                    ->shader(cF, e2::form::state::focus::count, data_src_sptr)
                    ->shader(cell::shaders::xlight, e2::form::state::hover)
                    ->invoke([&](auto& boss)
                    {
                        auto& data_shadow = boss.base::field(ptr::shadow(data_src_sptr));
                        auto& data_src_id = boss.base::field(data_src_sptr->id);
                        data_sources.insert(data_src_id);
                        boss.depend(data_src_sptr);
                        data_src_sptr->LISTEN(tier::release, e2::form::prop::ui::header, new_title, boss.sensors)
                        {
                            boss.set(label_format(new_title));
                            client->resize();
                        };
                        boss.LISTEN(tier::release, e2::form::upon::vtree::attached, parent)
                        {
                            parent->resize();
                        };
                        boss.LISTEN(tier::release, e2::form::upon::vtree::detached, parent)
                        {
                            data_sources.erase(data_src_id);
                            parent->resize(); // Rebuild list.
                        };
                        data_src_sptr->LISTEN(tier::release, tile::events::delist, f, boss.sensors)
                        {
                            boss.base::detach(); // Destroy itself.
                        };
                        boss.on(tier::mouserelease, input::key::MouseAny, [&](hids& gear)
                        {
                            if ((gear.cause & 0x00FF) && !gear.dragged) // Button events only.
                            if (auto data_ptr = data_shadow.lock())
                            {
                                auto& data_src = *data_ptr;
                                gear.forward(tier::mouserelease, data_src);
                                gear.dismiss();
                            }
                        });
                        boss.LISTEN(tier::release, e2::form::state::mouse, hovered)
                        {
                            if (auto data_ptr = data_shadow.lock())
                            {
                                data_ptr->base::signal(tier::release, e2::form::state::highlight, hovered);
                            }
                        };
                        boss.base::resize(); // Update item's size (recalc size to apply setpad{ 1, 1 }).
                    });
            };
            boss.LISTEN(tier::release, e2::render::any, parent_canvas, memo)
            {
                if (window_state == winstate::normal)
                {
                    auto context2D = parent_canvas.bump({ 0, si32max / 2, 0, si32max / 2 });
                    client->render(parent_canvas);
                    parent_canvas.bump(context2D);
                }
            };
            boss.LISTEN(tier::anycast, e2::form::upon::started, root_ptr, memo)
            {
                data_sources.clear();
                client->clear();
                if (auto parent_ptr = boss.base::parent())
                {
                    window_state = parent_ptr->base::riseup(tier::request, e2::form::prop::window::state);
                }
            };
        }
    };

    namespace
    {
        auto mouse_subs = [](auto& boss)
        {
            boss.on(tier::mouserelease, input::key::LeftDoubleClick, [&](hids& gear)
            {
                boss.base::riseup(tier::preview, e2::form::size::enlarge::maximize, gear);
                gear.dismiss();
            });
        };
        auto app_window = [](auto& what, auto slot_ptr, auto focus_history_ptr)
        {
            auto base_state = what.type == netxs::app::tile::id ? winstate::tiled
                                                                : winstate::normal;
            return track_slot_focus(ui::fork::ctor(axis::Y)
                    ->template plugin<pro::title>(what.applet->base::property("applet.header"), what.applet->base::property("applet.footer"), true, faux, true)
                    ->template plugin<pro::light>() //todo gcc requires template keyword
                    ->template plugin<pro::focus>()
                    ->limits({ 5, -1 }, { -1, -1 })
                    ->isroot(true)
                    ->active()
                    ->invoke([&](auto& boss)
                    {
                        auto& pane_state = boss.base::field(base_state);
                        auto pane_state_value = [&boss, base_state]
                        {
                            auto state = base_state;
                            if (auto parent_ptr = boss.base::parent())
                            {
                                if (parent_ptr->base::subset.size() > 2)
                                {
                                    state = winstate::maximized;
                                }
                            }
                            return state;
                        };
                        auto sync_pane_state = [&boss, &pane_state, pane_state_value](bool forced = faux)
                        {
                            auto state = pane_state_value();
                            if (forced || pane_state != state)
                            {
                                pane_state = state;
                                boss.base::broadcast(tier::release, e2::form::prop::window::state, pane_state);
                            }
                        };
                        mouse_subs(boss);
                        if (what.applet->size() != dot_00) boss.resize(what.applet->size() + dot_01/*approx title height*/);
                        boss.on(tier::mouserelease, input::key::LeftDragStart, [&](hids& gear) { (void)gear; });
                        boss.on(tier::mouserelease, input::key::LeftRightDragStart);
                        boss.on(tier::mouserelease, input::key::RightClick, [&](hids& gear)
                        {
                            pro::focus::set(boss.This(), gear.id, solo::on);
                        });
                        boss.on(tier::mouserelease, input::key::MiddleClick);
                        boss.LISTEN(tier::anycast, e2::form::upon::started, root_ptr, -, (sync_pane_state))
                        {
                            boss.base::riseup(tier::release, tile::events::enlist, boss.This());
                            sync_pane_state(true);
                        };
                        boss.LISTEN(tier::request, e2::form::prop::window::statesrc, window_ptr)
                        {
                            window_ptr = boss.This();
                        };
                        boss.LISTEN(tier::request, e2::form::prop::window::state, state, -, (pane_state_value))
                        {
                            state = pane_state_value();
                        };
                        boss.LISTEN(tier::release, e2::form::upon::vtree::attached, parent, -, (sync_pane_state))
                        {
                            sync_pane_state();
                            parent->LISTEN(tier::anycast, e2::form::prop::cwd, path, boss.relyon)
                            {
                                boss.base::signal(tier::anycast, e2::form::prop::cwd, path);
                            };
                        };
                    })
                    ->branch(slot::_1, ui::postfx<cell::shaders::contrast>::ctor()
                        ->upload(what.applet->base::property("applet.header"))
                        ->shader(cell::shaders::text(cell{ whitespace }))
                        ->invoke([&](auto& boss)
                        {
                            boss.LISTEN(tier::release, e2::render::any, parent_canvas)
                            {
                                // Fill header background with color from config (shadower color)
                                parent_canvas.fill([](cell& c)
                                {
                                    c.bgc(skin::color(tone::shadower).bgc());
                                });
                            };
                            boss.LISTEN(tier::release, e2::form::upon::vtree::attached, parent)
                            {
                                auto shadow = ptr::shadow(boss.This());
                                parent->LISTEN(tier::release, e2::form::prop::ui::title, head_foci, -, (shadow))
                                {
                                    if (auto boss_ptr = shadow.lock())
                                    {
                                        boss_ptr->upload(head_foci);
                                    }
                                };
                            };
                        }))
                    ->branch(slot::_2, what.applet), slot_ptr, focus_history_ptr);
        };
        auto build_node = [](auto tag, auto slot1, auto slot2, auto grip_width, auto grip_bindings_ptr)
        {
            auto highlight_color = skin::color(tone::winfocus);
            auto c3 = highlight_color;

            auto node = tag == 'h' ? ui::fork::ctor(axis::X, grip_width == -1 ? 2 : grip_width, slot1, slot2)
                                   : ui::fork::ctor(axis::Y, grip_width == -1 ? 1 : grip_width, slot1, slot2);
            node->isroot(faux, base::node) // Set object kind to 1 to be different from others. See node_veer::select.
                ->template plugin<pro::focus>()
                ->limits(dot_00)
                ->invoke([&](auto& boss)
                {
                    mouse_subs(boss);
                    boss.on(tier::mouserelease, input::key::MouseWheel, [&](hids& gear)
                    {
                        if (gear.meta(hids::anyCtrl))
                        {
                            boss.move_slider(gear.whlsi);
                            gear.dismiss();
                        }
                    });
                    boss.LISTEN(tier::release, app::tile::events::ui::swap     , gear) { boss.swap();       };
                    boss.LISTEN(tier::release, app::tile::events::ui::rotate   , gear) { boss.rotate();     };
                    boss.LISTEN(tier::release, app::tile::events::ui::equalize , gear) { boss.config(1, 1); };
                    boss.LISTEN(tier::preview, app::tile::events::ui::grips::move, delta)
                    {
                        if (delta)
                        {
                            auto [orientation, griparea, ratio] = boss.get_config();
                            auto step = orientation == axis::X ? delta.x : delta.y;
                            if (step == 0) boss.bell::passover();
                            else           boss.move_slider(step);
                        }
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::grips::resize, step)
                    {
                        if (step)
                        {
                            auto [orientation, griparea, ratio] = boss.get_config();
                            auto grip_width = orientation == axis::X ? griparea.size.x : griparea.size.y;
                            boss.set_grip_width(grip_width + step);
                        }
                    };
                });
                auto grip = node->attach(slot::_I, ui::mock::ctor()
                    ->isroot(true)
                    ->active()
                    ->template plugin<pro::mouse>()
                    ->template plugin<pro::mover>()
                    ->template plugin<pro::focus>(pro::focus::mode::focusable)
                    ->template plugin<pro::keybd>()
                    ->shader(c3, e2::form::state::focus::count)
                    ->template plugin<pro::shade<cell::shaders::xlight>>()
                    ->invoke([&](auto& boss)
                    {
                        auto& is_focused = boss.base::field(faux);
                        // Track focus state
                        boss.LISTEN(tier::release, e2::form::state::focus::count, count)
                        {
                            is_focused = !!count;
                            boss.base::deface();
                        };
                        // Set default dark gray background for non-focused grip
                        boss.LISTEN(tier::release, e2::render::any, parent_canvas)
                        {
                            if (!is_focused)  // Not focused
                            {
                                parent_canvas.fill([](cell& c)
                                {
                                    c.bgc(0xff202020);  // Dark gray #202020
                                });
                            }
                        };
                        boss.on(tier::mouserelease, input::key::RightClick, [&](hids& gear)
                        {
                            boss.base::riseup(tier::preview, e2::form::size::minimize, gear);
                            gear.dismiss();
                        });
                        auto& luafx = boss.bell::indexer.luafx;
                        auto& bindings = *grip_bindings_ptr;
                        input::bindings::keybind(boss, bindings);
                        boss.base::add_methods(basename::grip, //todo self_hosted?
                        {
                            { methods::MoveGrip,        [&]
                                                        {
                                                            auto delta = luafx.get_args_or(1, dot_00);
                                                            boss.base::riseup(tier::preview, app::tile::events::ui::grips::move, delta);
                                                            auto& gear = luafx.get_gear();
                                                            gear.set_handled();
                                                            luafx.set_return();
                                                        }},
                            { methods::ResizeGrip,      [&]
                                                        {
                                                            auto delta = luafx.get_args_or(1, si32{});
                                                            boss.base::riseup(tier::preview, app::tile::events::ui::grips::resize, delta);
                                                            auto& gear = luafx.get_gear();
                                                            gear.set_handled();
                                                            luafx.set_return();
                                                        }},
                            { methods::FocusNextGrip,   [&]
                                                        {
                                                            auto& gear = luafx.get_gear();
                                                            auto ok = gear.is_real();
                                                            if (ok)
                                                            {
                                                                auto delta = luafx.get_args_or(1, si32{ 1 });
                                                                delta > 0 ? boss.base::riseup(tier::preview, app::tile::events::ui::focus::nextgrip, gear)
                                                                          : boss.base::riseup(tier::preview, app::tile::events::ui::focus::prevgrip, gear);
                                                                gear.set_handled();
                                                            }
                                                            luafx.set_return(ok);
                                                        }},
                        });
                    }));
            return node;
        };
        auto empty_slot = [](auto slot_ptr, auto focus_history_ptr)
        {
            auto window_clr = skin::color(tone::window_clr);
            window_clr.bga(0x60);
            auto highlight_color = skin::color(tone::winfocus);
            auto danger_color    = skin::color(tone::danger);
            auto c3 = highlight_color.bga(0x40);
            auto c1 = danger_color;

            using namespace app::shared;
            auto [menu_block, cover, menu_data] = menu::mini(true, faux, 1,
            menu::list
            {
                { menu::item{ .alive = true, .label = "  +  ", .tooltip = " Launch application instance.                            \n"
                                                                          " The app to run can be set by RightClick on the taskbar. " },
                [](auto& boss, auto& /*item*/)
                {
                    boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
                    {
                        pro::focus::set(boss.This(), gear.id, solo::on);
                        boss.base::riseup(tier::request, e2::form::proceed::createby, gear);
                        gear.dismiss(true);
                    });
                }},
                { menu::item{ .alive = true, .label = " [|] ", .tooltip = " Split horizontally " },
                [](auto& boss, auto& /*item*/)
                {
                    boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
                    {
                        boss.base::riseup(tier::release, app::tile::events::ui::split::hz, gear);
                        gear.dismiss(true);
                    });
                }},
                { menu::item{ .alive = true, .label = " [─] ", .tooltip = " Split vertically " },
                [](auto& boss, auto& /*item*/)
                {
                    boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
                    {
                        boss.base::riseup(tier::release, app::tile::events::ui::split::vt, gear);
                        gear.dismiss(true);
                    });
                }},
                { menu::item{ .alive = true, .label = "  ×  ", .tooltip = " Delete pane ", .hover = c1 },
                [](auto& boss, auto& /*item*/)
                {
                    boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
                    {
                        boss.base::riseup(tier::release, e2::form::proceed::quit::one, true);
                        gear.dismiss(true);
                    });
                }},
            });
            menu_data->active(window_clr);
            auto menu_id = menu_block->id;
            cover->setpad({ 0, 0, 3, 0 });
            cover->invoke([&](auto& boss)
            {
                boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (menu_id))
                {
                    parent_canvas.fill([&](cell& c){ c.txt(whitespace).link(menu_id); });
                };
            });

            return track_slot_focus(ui::cake::ctor()
                ->isroot(true, base::placeholder)
                ->limits(dot_00, -dot_11)
                ->plugin<pro::focus>(pro::focus::mode::focusable)
                ->invoke([&](auto& boss)
                {
                    mouse_subs(boss);
                    auto& default_color = boss.base::field(window_clr).link(boss.id);
                    auto& hilight_color = boss.base::field(highlight_color).alpha(0x70).link(boss.id);
                    auto& current_color = boss.base::field(default_color);
                    boss.shader(current_color)
                        ->shader(c3, e2::form::state::focus::count);
                    auto& highlight = boss.base::field([&](auto state)
                    {
                        current_color = state ? hilight_color : default_color;
                        boss.base::deface();
                    });
                    boss.LISTEN(tier::release, vtm::events::d_n_d::abort, target)
                    {
                        highlight(faux);
                    };
                    boss.LISTEN(tier::release, vtm::events::d_n_d::ask, target)
                    {
                        if (auto parent_ptr = boss.base::parent())
                        if (parent_ptr->base::subset.size() == 1) // Only empty slot available.
                        {
                            highlight(true);
                            target = boss.This();
                        }
                    };
                    boss.LISTEN(tier::release, vtm::events::d_n_d::drop, what, -, (focus_history_ptr))
                    {
                        if (auto parent_ptr = boss.base::parent())
                        if (parent_ptr->base::subset.size() == 1) // Only empty slot available.
                        {
                            highlight(faux);
                            // Solo focus will be set in pro::d_n_d::proceed.
                            //pro::focus::off(boss.back()); // Unset focus from node_veer if it is focused.
                            auto app = app_window(what, parent_ptr, focus_history_ptr);
                            parent_ptr->attach(app);
                            app->base::broadcast(tier::anycast, e2::form::upon::started);
                            app->base::reflow();
                        }
                    };
                    boss.on(tier::mouserelease, input::key::RightClick, [&](hids& gear)
                    {
                        pro::focus::set(boss.This(), gear.id, solo::on);
                        boss.base::riseup(tier::request, e2::form::proceed::createby, gear);
                        gear.dismiss(true);
                    });
                })
                ->branch
                (
                    ui::post::ctor()->upload("Empty Slot", 10)
                        ->limits({ 10, 1 }, { 10, 1 })
                        ->alignment({ snap::center, snap::center })
                )
                ->branch
                (
                    menu_block->alignment({ snap::head, snap::head })
                ), slot_ptr, focus_history_ptr);
        };
        auto node_veer = [](auto&& node_veer, [[maybe_unused]] auto min_state, auto grip_bindings_ptr, auto focus_history_ptr, auto confirm_block = netxs::sptr<bool>{}) -> netxs::sptr<ui::veer>
        {
            auto slot_ptr = ui::veer::ctor()
                ->plugin<pro::focus>()
                ->active()
                ->invoke([&](auto& boss)
                {
                    boss.LISTEN(tier::release, e2::config::plugins::sizer::alive, state)
                    {
                        // Block a rising up of this event: dtvt object fires this event on exit.
                    };
                    boss.LISTEN(tier::release, e2::form::proceed::swap, item_ptr, -, (focus_history_ptr))
                    {
                        if (boss.count() == 1) // Only empty slot available.
                        {
                            if constexpr (debugmode) log(prompt::tile, "Empty slot swap: defective structure, count=", boss.count());
                        }
                        else if (boss.count() == 2)
                        {
                            auto gear_id_list = pro::focus::cut(boss.back());
                            auto deleted_item = boss.pop_back();
                            if (item_ptr)
                            {
                                if (focus_history_ptr)
                                {
                                    if (auto old_slot_ptr = focus_history_ptr->owner(item_ptr))
                                    {
                                        focus_history_ptr->replace(old_slot_ptr, boss.This());
                                    }
                                    focus_history_ptr->bind(item_ptr, boss.This());
                                }
                                input::hids::cleanup(*item_ptr);
                                boss.attach(item_ptr);
                                item_ptr->base::broadcast(tier::anycast, e2::form::upon::started);
                            }
                            else item_ptr = boss.This();
                            pro::focus::set(boss.back(), gear_id_list, solo::off);
                        }
                        else
                        {
                            if constexpr (debugmode) log(prompt::tile, "Empty slot swap: defective structure, count=", boss.count());
                        }
                    };
                    boss.LISTEN(tier::release, e2::form::upon::vtree::attached, parent)
                    {
                        parent->LISTEN(tier::request, e2::form::proceed::swap, item_ptr, boss.relyon)
                        {
                            if (item_ptr != boss.This())
                            {
                                if (boss.count() == 1) // Only empty slot available.
                                {
                                    item_ptr.reset();
                                }
                                else if (boss.count() == 2)
                                {
                                    auto gear_id_list = pro::focus::cut(boss.back());
                                    item_ptr = boss.pop_back();
                                    pro::focus::set(boss.back(), gear_id_list, solo::off);
                                }
                                else
                                {
                                    if constexpr (debugmode) log(prompt::tile, "Empty slot: defective structure, count=", boss.count());
                                }
                                if (auto parent = boss.base::parent())
                                {
                                    parent->bell::expire();
                                }
                            }
                        };
                    };
                    boss.LISTEN(tier::preview, e2::form::size::minimize, gear, - /*, (saved_ratio = 1, min_ratio = 1, min_state)*/)
                    {
                        if (boss.count() > 2) // Restore if maximized.
                        {
                            boss.back()->base::signal(tier::release, e2::form::size::restore);
                        }
                        // else if (auto node = std::static_pointer_cast<ui::fork>(boss.base::parent()))
                        // {
                        //     auto ratio = node->get_ratio();
                        //     if (ratio == min_ratio)
                        //     {
                        //         node->set_ratio(saved_ratio);
                        //         pro::focus::set(boss.This(), gear.id, gear.meta(hids::anyCtrl) ? solo::off : solo::on, true);
                        //     }
                        //     else
                        //     {
                        //         saved_ratio = ratio;
                        //         node->set_ratio(min_state);
                        //         min_ratio = node->get_ratio();
                        //         pro::focus::off(boss.This(), gear.id);
                        //     }
                        //     node->base::reflow();
                        // }
                        pro::focus::set(boss.back(), gear.id, solo::on, true);
                    };
                    boss.LISTEN(tier::preview, e2::form::size::enlarge::any, gear, -, (oneoff = subs{}))
                    {
                        pro::focus::set(boss.This(), gear.id, solo::off);
                        if (boss.count() > 2 || oneoff.size()) // It is a root or is already maximized. See build_inst::slot::_2's e2::form::proceed::attach for details.
                        {
                            boss.base::riseup(tier::release, e2::form::proceed::attach);
                        }
                        else
                        {
                            if (boss.count() > 1) // Preventing the empty slot from maximizing.
                            if (boss.back()->base::kind() == base::client) // Preventing the splitter from maximizing.
                            {
                                auto fullscreen_item = boss.back();
                                auto& fullscreen_inst = *fullscreen_item;
                                pro::focus::set(fullscreen_item, gear.id, solo::on, true);
                                boss.base::riseup(tier::release, e2::form::proceed::attach, fullscreen_item);
                                fullscreen_item->LISTEN(tier::release, e2::form::size::restore, p, oneoff)
                                {
                                    auto item_ptr = fullscreen_inst.This();
                                    auto gear_id_list = pro::focus::cut(item_ptr);
                                    item_ptr->base::detach();
                                    boss.attach(item_ptr);
                                    item_ptr->base::broadcast(tier::anycast, e2::form::upon::started);
                                    pro::focus::set(item_ptr, gear_id_list, solo::off);
                                    boss.base::reflow();
                                    oneoff.clear();
                                };
                                fullscreen_item->LISTEN(tier::release, e2::form::upon::vtree::detached, parent_ptr, oneoff)
                                {
                                    oneoff.clear();
                                };
                                boss.base::reflow();
                            }
                        }
                    };
                    boss.LISTEN(tier::release, app::tile::events::ui::split::any, gear, -, (grip_bindings_ptr, focus_history_ptr, confirm_block))
                    {
                        auto deed = boss.bell::protos();
                        auto depth = 0;
                        auto parent_ptr = boss.base::parent();
                        while (parent_ptr)
                        {
                            depth++;
                            parent_ptr = parent_ptr->base::parent();
                        }
                        if constexpr (debugmode) log(prompt::tile, "Depth ", depth);
                        if (depth > inheritance_limit) return;

                        auto heading = deed == app::tile::events::ui::split::vt.id;
                        auto newnode = build_node(heading ? 'v':'h', 1, 1, heading ? 1 : 2, grip_bindings_ptr);
                        auto empty_1 = node_veer(node_veer, ui::fork::min_ratio, grip_bindings_ptr, focus_history_ptr, confirm_block);
                        auto empty_2 = node_veer(node_veer, ui::fork::max_ratio, grip_bindings_ptr, focus_history_ptr, confirm_block);
                        auto gear_id_list = pro::focus::cut(boss.back());
                        auto curitem = boss.pop_back();
                        if (boss.empty())
                        {
                            boss.attach(empty_slot(boss.This(), focus_history_ptr));
                            empty_1->pop_back();
                        }
                        auto slot_1 = newnode->attach(slot::_1, empty_1->branch(curitem));
                        auto slot_2 = newnode->attach(slot::_2, empty_2);
                        boss.attach(newnode);
                        if (focus_history_ptr)
                        {
                            focus_history_ptr->replace(boss.This(), slot_1);
                            focus_history_ptr->bind(curitem, slot_1);
                        }
                        newnode->base::broadcast(tier::anycast, e2::form::upon::started);
                        slot_2->base::signal(tier::request, e2::form::proceed::createby, gear);
                    };
                    boss.LISTEN(tier::anycast, e2::form::proceed::quit::any, fast, -, (confirm_block))
                    {
                        if (confirm_block && *confirm_block) return; // Blocked by pending close confirmation dialog.
                        boss.base::signal(tier::preview, e2::form::proceed::quit::one, fast);
                        boss.base::signal(tier::general, e2::shutdown, utf::concat(prompt::tile, "Shutdown on signal"));
                    };
                    boss.LISTEN(tier::preview, e2::form::proceed::quit::one, fast)
                    {
                        if (boss.count() > 1 && boss.back()->base::root()) // Walking a nested visual tree.
                        {
                            boss.back()->base::signal(tier::anycast, e2::form::proceed::quit::one, true); // fast=true: Immediately closing (no ways to showing a closing process). Forward a quit message to hosted app in order to schedule a cleanup.
                        }
                        else // Close an empty slot (boss.count() == 1).
                        {
                            boss.base::enqueue([&](auto& /*boss*/) // Enqueue to keep the focus tree intact while processing events.
                            {
                                boss.base::signal(tier::release, e2::form::proceed::quit::one, fast);
                            });
                        }
                        boss.bell::expire(); // Stop riseup: quit has been handled by this node_veer.
                    };
                    boss.LISTEN(tier::release, e2::form::proceed::quit::any, fast)
                    {
                        // Restore maximized windows before processing quit to preserve focus
                        if (boss.count() > 2) // Window is maximized
                        {
                            auto item_ptr = boss.back();
                            boss.base::riseup(tier::release, e2::form::proceed::attach); // Restore the window before quit.
                            if (boss.count() <= 2 || boss.back() != item_ptr)
                            {
                                item_ptr->base::riseup(tier::release, e2::form::proceed::quit::one, fast);
                                boss.bell::expire(); // Stop riseup: quit has been re-dispatched after maximize restore.
                                return;
                            }
                        }
                        if (auto parent = boss.base::parent())
                        {
                            if (boss.count() > 1 && boss.back()->base::kind() == base::client) // Only apps can be deleted.
                            {
                                auto gear_id_list = pro::focus::cut(boss.back());
                                auto deleted_item = boss.pop_back(); // Throw away.
                                pro::focus::set(boss.back(), gear_id_list, solo::off);
                            }
                            else if (boss.count() == 1) // Remove empty slot, reorganize.
                            {
                                auto item_ptr = parent->base::signal(tier::request, e2::form::proceed::swap, boss.This()); // sptr must be of the same type as the event argument. Casting kills all intermediaries when return.
                                if (item_ptr != boss.This()) // Parallel slot is not empty or both slots are empty (item_ptr == null).
                                {
                                    parent->base::riseup(tier::release, e2::form::proceed::swap, item_ptr);
                                }
                            }
                            boss.base::broadcast(tier::anycast, e2::form::upon::started);
                            boss.base::deface();
                            boss.base::reflow();
                        }
                        boss.bell::expire(); // Stop riseup: quit has been handled by this node_veer.
                    };
                    boss.LISTEN(tier::request, e2::form::proceed::createby, gear, -, (focus_history_ptr))
                    {
                        if (boss.count() != 1) return; // Create new apps at the empty slots only.
                        auto& gate = gear.owner;
                        auto& current_default = gate.base::property("desktop.selected");
                        if (auto world_ptr = boss.base::signal(tier::general, e2::config::creator)) // Finalize app creation.
                        {
                            auto what = world_ptr->base::signal(tier::request, vtm::events::apptype, { .menuid = current_default });
                            if (what.type == netxs::app::site::id) return; // Deny any desktop viewport markers inside the tiling manager.
                            world_ptr->base::signal(tier::request, vtm::events::newapp, what);
                            auto app = app_window(what, boss.This(), focus_history_ptr);
                            pro::focus::off(boss.back());
                            boss.attach(app);
                            app->base::signal(tier::anycast, vtm::events::attached, world_ptr);
                            auto root_ptr = what.applet;
                            app->base::broadcast(tier::anycast, e2::form::upon::started, root_ptr);
                            pro::focus::set(app, gear.id, solo::off);
                        }
                        else // Standalone mode: Create app directly without desktop environment.
                        {
                            auto& indexer = ui::tui_domain();
                            auto& config = indexer.config;
                            auto tile_app_context = config.settings::push_context("/config/tile/app");
                            auto default_selected_id = config.settings::take("/config/tile/app/selected", "term"s);
                            // Try to find tile.selected property by traversing up the parent chain
                            text selected_id = default_selected_id;
                            auto current_ptr = boss.base::This();
                            while (current_ptr)
                            {
                                auto& prop = current_ptr->base::property("tile.selected");
                                if (!prop.empty())
                                {
                                    selected_id = prop;
                                    break;
                                }
                                auto parent = current_ptr->base::parent();
                                if (!parent || parent == current_ptr) break;
                                current_ptr = parent;
                            }
                            auto item_list = config.settings::take_ptr_list_for_name("item");
                            text app_type;
                            text cmd;
                            text menuid;
                            text title;
                            for (auto& item_ptr : item_list)
                            {
                                auto item_id = config.settings::take_value_from(item_ptr, "id", text{});
                                if (item_id == selected_id)
                                {
                                    app_type = config.settings::take_value_from(item_ptr, "type", text{});
                                    cmd = config.settings::take_value_from(item_ptr, "cmd", text{});
                                    title = config.settings::take_value_from(item_ptr, "title", text{});
                                    menuid = item_id;
                                    break;
                                }
                            }
                            if (app_type.empty()) app_type = "dtvt";
                            if (cmd.empty()) cmd = "$0 -r term";
                            if (menuid.empty()) menuid = selected_id;
                            auto appcfg = eccc{ .cmd = cmd };
                            expand_appcfg(appcfg);
                            auto applet = app::shared::builder(app_type)(appcfg, config);
                            set_pane_title(applet, title);
                            auto what = vtm::events::handoff.param();
                            what.applet = applet;
                            what.type = app_type;
                            what.menuid = menuid;
                            auto app = app_window(what, boss.This(), focus_history_ptr);
                            pro::focus::off(boss.back());
                            boss.attach(app);
                            auto root_ptr = what.applet;
                            app->base::broadcast(tier::anycast, e2::form::upon::started, root_ptr);
                            pro::focus::set(app, gear.id, solo::off);
                        }
                    };
                    //todo unify, demo limits
                    //static auto insts_count = 0;
                    //insts_count++;
                    //boss.LISTEN(tier::release, e2::form::upon::vtree::detached, parent_ptr)
                    //{
                    //    insts_count--;
                    //    if constexpr (debugmode) log(prompt::tile, "Instance detached: id:", id, "; left:", insts_count);
                    //};
                });
            slot_ptr->attach(empty_slot(slot_ptr, focus_history_ptr));
            return slot_ptr;
        };
        auto parse_data = [](auto&& parse_data, view& utf8, auto min_ratio, auto grip_bindings_ptr, auto focus_history_ptr, auto confirm_block = netxs::sptr<bool>{}, text selected_id_override = {}) -> netxs::sptr<ui::veer>
        {
            auto slot_ptr = node_veer(node_veer, min_ratio, grip_bindings_ptr, focus_history_ptr, confirm_block);
            utf::trim_front(utf8, ", ");
            if (utf8.empty())
            {
                auto& indexer = ui::tui_domain();
                auto& config = indexer.config;
                auto tile_app_context = config.settings::push_context("/config/tile/app");
                auto selected_id = selected_id_override.empty() ? config.settings::take("/config/tile/app/selected", "term"s)
                                                                : selected_id_override;
                auto item_list = config.settings::take_ptr_list_for_name("item");
                text app_type;
                text cmd;
                text menuid;
                text title;
                for (auto& item_ptr : item_list)
                {
                    auto item_id = config.settings::take_value_from(item_ptr, "id", text{});
                    if (item_id == selected_id)
                    {
                        app_type = config.settings::take_value_from(item_ptr, "type", text{});
                        cmd = config.settings::take_value_from(item_ptr, "cmd", text{});
                        title = config.settings::take_value_from(item_ptr, "title", text{});
                        menuid = item_id;
                        break;
                    }
                }
                if (app_type.empty()) app_type = "dtvt";
                if (cmd.empty()) cmd = "$0 -r term";
                if (menuid.empty()) menuid = selected_id;
                auto appcfg = eccc{ .cmd = cmd };
                expand_appcfg(appcfg);
                auto applet = app::shared::builder(app_type)(appcfg, config);
                set_pane_title(applet, title);
                auto what = vtm::events::handoff.param();
                what.applet = applet;
                what.type = app_type;
                what.menuid = menuid;
                auto app = app_window(what, slot_ptr, focus_history_ptr);
                if (slot_ptr->count()) pro::focus::off(slot_ptr->back());
                slot_ptr->attach(app);
                auto root_ptr = what.applet;
                app->base::broadcast(tier::anycast, e2::form::upon::started, root_ptr);
                return slot_ptr;
            }
            auto tag = utf8.front();
            if ((tag == 'h' || tag == 'v') && utf8.find('(') < utf8.find(','))
            {
                // add split
                utf8.remove_prefix(1);
                utf::trim_front(utf8, ' ');
                auto s1 = si32{ 1 };
                auto s2 = si32{ 1 };
                auto w  = si32{-1 };
                if (auto l = utf::to_int(utf8)) // Left side ratio
                {
                    s1 = std::abs(l.value());
                    if (utf8.empty() || utf8.front() != ':') return slot_ptr;
                    utf8.remove_prefix(1);
                    if (auto r = utf::to_int(utf8)) // Right side ratio
                    {
                        s2 = std::abs(r.value());
                        utf::trim_front(utf8, ' ');
                        if (!utf8.empty() && utf8.front() == ':') // Grip width.
                        {
                            utf8.remove_prefix(1);
                            if (auto g = utf::to_int(utf8))
                            {
                                w = std::abs(g.value());
                                utf::trim_front(utf8, ' ');
                            }
                        }
                    }
                    else return slot_ptr;
                }
                if (utf8.empty() || utf8.front() != '(') return slot_ptr;
                utf8.remove_prefix(1);
                auto node = build_node(tag, s1, s2, w, grip_bindings_ptr);
                auto slot1 = node->attach(slot::_1, parse_data(parse_data, utf8, ui::fork::min_ratio, grip_bindings_ptr, focus_history_ptr, confirm_block, selected_id_override));
                auto slot2 = node->attach(slot::_2, parse_data(parse_data, utf8, ui::fork::max_ratio, grip_bindings_ptr, focus_history_ptr, confirm_block, selected_id_override));
                slot_ptr->attach(node);
                utf::trim_front(utf8, ") ");
            }
            else  // Add application.
            {
                utf::trim_front(utf8, ' ');
                auto menuid = utf::take_front(utf8, " ,)").str();
                if (menuid.empty()) return slot_ptr;

                utf::trim_front(utf8, " ,");
                if (utf8.size() && utf8.front() == ')') utf8.remove_prefix(1); // pop ')';

                auto& s = *slot_ptr;
                auto& oneshot = s.base::field(hook{});
                s.LISTEN(tier::anycast, vtm::events::attached, world_ptr, oneshot, (menuid, focus_history_ptr))
                {
                    auto what = world_ptr->base::signal(tier::request, vtm::events::newapp, { .menuid = menuid });
                    auto inst_ptr = app_window(what, s.This(), focus_history_ptr);
                    s.attach(inst_ptr);
                    inst_ptr->base::signal(tier::anycast, vtm::events::attached, world_ptr);
                    s.base::unfield(oneshot);
                };
            }
            return slot_ptr;
        };
        namespace item_type
        {
            static constexpr auto _counter   = __COUNTER__ + 1;
            static constexpr auto empty_slot = __COUNTER__ - _counter;
            static constexpr auto applet     = __COUNTER__ - _counter;
            static constexpr auto grip       = __COUNTER__ - _counter;
        }
        auto _foreach = [](auto _foreach, sptr& root_veer_ptr, id_t gear_id, auto proc) -> void
        {
            if (auto node_veer_ptr = std::static_pointer_cast<ui::veer>(root_veer_ptr))
            {
                if (pro::focus::is_focused(node_veer_ptr, gear_id))
                {
                    auto item_ptr = node_veer_ptr->back();
                    if (node_veer_ptr->count() == 1) // Empty slot.
                    {
                        if (pro::focus::is_focused(item_ptr, gear_id))
                        {
                            proc(item_ptr, item_type::empty_slot, node_veer_ptr);
                            if (!item_ptr)
                            {
                                root_veer_ptr = {}; // Interrupt foreach.
                            }
                        }
                    }
                    else if (item_ptr->root()) // App window (ui::fork).
                    {
                        if (auto applet_host_ptr = std::static_pointer_cast<ui::fork>(item_ptr))
                        if (auto applet_ptr = applet_host_ptr->get(slot::_2))
                        if (pro::focus::is_focused(applet_ptr, gear_id))
                        {
                            proc(applet_ptr, item_type::applet, node_veer_ptr); // Applet.
                            if (!applet_ptr)
                            {
                                root_veer_ptr = {}; // Interrupt foreach.
                            }
                        }
                    }
                    else // if (!item_ptr->root()) // Node (ui::fork).
                    {
                        if (auto veer_host_ptr = std::static_pointer_cast<ui::fork>(item_ptr))
                        {
                            root_veer_ptr = veer_host_ptr->get(slot::_1);
                            _foreach(_foreach, root_veer_ptr, gear_id, proc);
                            if (!root_veer_ptr) return;
                            if (auto grip_ptr = veer_host_ptr->get(slot::_I))
                            {
                                if (pro::focus::is_focused(grip_ptr, gear_id))
                                {
                                    proc(grip_ptr, item_type::grip, node_veer_ptr); // Grip.
                                    if (!grip_ptr)
                                    {
                                        root_veer_ptr = {}; // Interrupt foreach.
                                        return;
                                    }
                                }
                            }
                            root_veer_ptr = veer_host_ptr->get(slot::_2);
                            _foreach(_foreach, root_veer_ptr, gear_id, proc);
                            if (!root_veer_ptr) return;
                        }
                    }
                }
            }
        };
        auto build_inst = [](eccc appcfg, settings& config) -> sptr
        {
            // tile (ui::fork, f, k)
            //  │ │
            //  │ └─ slot::_1 ─ menu_block
            //  └─── slot::_2 ─ parse_data
            //        │
            //        └─ node_veer (ui::veer, f)
            //            │
            //            ├─ empty_slot (ui::cake, f)->isroot(true, base::placeholder)
            //            │   │
            //            │   ├─ ui::post ("Empty Slot")
            //            │   └─ menu (" +  |  ─  x ")
            //
            //            └─ app_window (ui::fork, f)->isroot(true)
            //                │ │
            //           or   │ └─ slot::_1: ui::postfx<cell::shaders::contrast>("Title")
            //                └─── slot::_2: what.applet
            //            └─ node (ui::fork, f)->isroot(faux, base::node)
            //                │ │ │
            //                │ │ └─ slot::_1: parse_data...
            //                │ └─── slot::_I: (ui::mock, f)->isroot(true)
            //                └───── slot::_2: parse_data...
            //            :
            //            └─ maximized node_veer...

            auto param = view{ appcfg.cmd };
            //auto highlight_color = skin::color(tone::highlight);
            //auto danger_color    = skin::color(tone::danger);
            //auto warning_color   = skin::color(tone::warning);
            //auto c3 = highlight_color;
            //auto c2 = warning_color;
            //auto c1 = danger_color;

            // Wrap in a cake to support overlay dialogs (e.g., close confirmation).
            // Both wrapper and object use mode::hub_boundary: they behave like
            // mode::hub on focus::set::on (don't cut sibling branches, so clicking
            // the menubar area doesn't steal focus from the active pane), while
            // acting as a hard boundary for unfocus riseup (focus::set::off preview),
            // preventing internal Ctrl+LeftClick unfocus signals from leaking out
            // and decrementing object's focus count to 0 (which would repaint the
            // menubar with the inactive palette even though the tile applet is
            // still the focused leaf at the gate level).
            auto wrapper = ui::cake::ctor()
                ->plugin<pro::focus>(pro::focus::mode::hub_boundary);
            auto object = wrapper->attach(ui::fork::ctor(axis::Y))
                ->plugin<items>()
                ->plugin<pro::focus>(pro::focus::mode::hub_boundary)
                ->plugin<pro::keybd>();
            auto& window_clr = object->base::field(skin::color(tone::window_clr));
            auto& is_focused = object->base::field(faux);
            auto& color_focused = object->base::field(skin::color(tone::winfocus));
            auto& color_passive = object->base::field(skin::color(tone::window_clr));
            object ->invoke([&](auto& boss)
            {
                boss.LISTEN(tier::release, e2::form::state::focus::count, count)
                {
                    if (std::exchange(is_focused, !!count) != is_focused)
                    {
                        boss.base::deface(); // Trigger to update pro::cache.
                        window_clr = is_focused ? color_focused
                                                : color_passive;
                    }
                };
            });
            using namespace app::shared;
            auto tile_context = config.settings::push_context("/config/events/tile/grip/");
            auto script_list = config.settings::take_ptr_list_for_name("script");
            auto grip_bindings_ptr = ptr::shared(input::bindings::load(config, script_list));
            auto focus_histories_ptr = ptr::shared(std::vector<netxs::sptr<focus_history_t>>{});
            tile_context = config.settings::push_context("/config/tile/");
            auto confirm_close = config.settings::take("confirm_close", faux);
            auto confirm_block = ptr::shared(faux); // Shared flag: set to true while close-confirmation dialog is shown.
            auto pane_index_active = ptr::shared(faux); // Shared flag: set to true while pane-index overlay is shown.
            auto command_bar_active = ptr::shared(faux); // Shared flag: set to true while command-bar overlay is shown.
            // When non-null, the next focus::commandbar event uses this list instead of loading the
            // default command list from config. This is how focus::pickapp re-uses the command-bar
            // overlay machinery to render an "app picker" populated from /config/tile/app/item*.
            auto pending_cmd_list_ptr = ptr::shared(netxs::sptr<std::vector<command_bar::item>>{});
            // Flags for the upcoming command-bar session (consumed alongside pending_cmd_list_ptr).
            // Callers set these bits before firing focus::commandbar to opt-in to extra shortcuts.
            auto pending_cmd_flags_ptr = ptr::shared(si32{ command_bar::flags::none });
            auto [menu_block, cover, menu_data] = menu::load(config);
            object->attach(slot::_1, menu_block);
            menu_data->active()
                ->shader(window_clr)
                ->plugin<pro::acryl>();
            auto menu_id = menu_block->id;
            cover->invoke([&](auto& boss)
            {
                auto bar = cell{ "▀"sv }.link(menu_id);
                boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (bar))
                {
                    auto fgc = window_clr.bgc();
                    parent_canvas.fill([&](cell& c){ c.fgc(fgc).txt(bar).link(bar); });
                };
            });
            if (appcfg.cwd.size())
            {
                auto err = std::error_code{};
                fs::current_path(appcfg.cwd, err);
                if (err) log("%%Failed to change current directory to '%cwd%', error code: %error%", prompt::tile, appcfg.cwd, err.value());
                else     log("%%Change current directory to '%cwd%'", prompt::tile, appcfg.cwd);
            }
            // Close-confirmation handler: intercept quit on the wrapper (cake)
            // which is visited FIRST in the anycast broadcast (before root_veer
            // and its child veers).  Set confirm_block to prevent all veers'
            // quit::any handlers from triggering e2::shutdown while the dialog
            // is shown.
            if (confirm_close)
            {
                wrapper->invoke([&, confirm_block](auto& boss)
                {
                        boss.LISTEN(tier::anycast, e2::form::proceed::quit::one, fast, -, (confirm_block))
                    {
                        if (!fast && !*confirm_block)
                        {
                            *confirm_block = true;
                            app::shared::show_close_confirmation(boss,
                                [&boss, confirm_block] // on_confirm
                                {
                                    *confirm_block = faux;
                                    // Re-signal with fast=true to reuse the original
                                    // shutdown flow (quit::any → preview quit::one
                                    // to clean up child processes → e2::shutdown).
                                    // fast=true bypasses this confirm handler (!fast).
                                    boss.base::signal(tier::anycast, e2::form::proceed::quit::one, true);
                                },
                                [confirm_block] // on_cancel
                                {
                                    *confirm_block = faux;
                                });
                        }
                    };
                });
            }
            // Workspace management.
            // Structure: object (fork Y) slot::_2 -> inner_fork (fork Y):
            //   slot::_1: workspace_host (veer) - hosts the current workspace's root veer.
            //   slot::_2: status_bar (mock)      - renders workspace index buttons (fixed 1-row height).
            // The `workspaces` vector owns all workspaces. Only the current one is attached to workspace_host.
            static constexpr auto ws_min_index = char{ 0x30 };
            static constexpr auto ws_max_index = char{ 0x7E };
            static constexpr auto ws_max_count = size_t(ws_max_index - ws_min_index + 1);
            static constexpr auto ws_btn_w     = si32{ 3 };
            auto workspaces_ptr        = ptr::shared(std::vector<netxs::sptr<ui::veer>>{});
            auto current_ws_index_ptr  = ptr::shared(size_t{ 0 });
            auto previous_ws_index_ptr = ptr::shared(size_t{ 0 });
            auto refresh_status_bar_fn = ptr::shared(std::function<void()>{[]{}});
            auto open_workspace_popup_fn = ptr::shared(std::function<void()>{[]{}}); // Opens the workspace preview popup (Win+Tab style); set when the status bar is built.

            // Factory: build a workspace root veer (parse_data result) with the root-fullscreen-attach listener.
            auto make_workspace_veer = [grip_bindings_ptr, focus_histories_ptr, confirm_block](view param_view, text selected_id_override = {}) -> netxs::sptr<ui::veer>
            {
                auto focus_history_ptr = ptr::shared(focus_history_t{});
                focus_histories_ptr->push_back(focus_history_ptr);
                auto veer = parse_data(parse_data, param_view, ui::fork::min_ratio, grip_bindings_ptr, focus_history_ptr, confirm_block, selected_id_override);
                veer->invoke([](auto& boss)
                {
                    boss.LISTEN(tier::release, e2::form::proceed::attach, fullscreen_item)
                    {
                        if (boss.count() > 2)
                        {
                            boss.back()->base::signal(tier::release, e2::form::size::restore);
                        }
                        if (fullscreen_item)
                        {
                            auto gear_id_list = pro::focus::cut(fullscreen_item);
                            fullscreen_item->base::detach();
                            pro::focus::off(boss.This());
                            boss.attach(fullscreen_item);
                            fullscreen_item->base::broadcast(tier::anycast, e2::form::upon::started);
                            pro::focus::set(fullscreen_item, gear_id_list, solo::on);
                        }
                    };
                    // When this workspace's last content is swapped away (both fork
                    // slots become empty), the standard node_veer swap handler sets
                    // item_ptr = boss.This() and stops.  This is normal: the workspace
                    // still has one empty slot remaining and should stay alive.
                    // Workspace destruction is handled by the quit::any release handler
                    // when the user explicitly closes the last empty slot (count == 1),
                    // which fires a tier::request swap to workspace_host.
                });
                return veer;
            };

            // Workspace host: directly attached to object slot::_2 (no status bar — the
            // workspace switcher button and app picker have been migrated to the tile menu).
            auto workspace_host_ptr = object->attach(slot::_2, ui::veer::ctor()
                ->plugin<pro::focus>());

            // Build initial workspace 0 (from appcfg.cmd param).
            auto workspace_0 = make_workspace_veer(view{ param });
            workspaces_ptr->push_back(workspace_0);
            workspace_host_ptr->attach(workspace_0);
            auto root_veer_ptr = workspace_0; // Kept for compatibility with downstream code.

            // Accessor for the currently active workspace veer.
            auto current_ws = [workspaces_ptr, current_ws_index_ptr]() -> netxs::sptr<ui::veer>
            {
                if (workspaces_ptr->empty()) return {};
                auto idx = std::min(*current_ws_index_ptr, workspaces_ptr->size() - 1);
                return (*workspaces_ptr)[idx];
            };

            // Accessor for the currently active workspace's focus history.
            auto current_focus_history = [focus_histories_ptr, current_ws_index_ptr]() -> netxs::sptr<focus_history_t>
            {
                if (focus_histories_ptr->empty()) return {};
                auto idx = std::min(*current_ws_index_ptr, focus_histories_ptr->size() - 1);
                return (*focus_histories_ptr)[idx];
            };

            // Switch active workspace to the given index.
            auto switch_workspace = [workspaces_ptr, current_ws_index_ptr, previous_ws_index_ptr, workspace_host_ptr, refresh_status_bar_fn, object_shadow = ptr::shadow(object)](size_t idx) -> bool
            {
                if (idx >= workspaces_ptr->size()) return faux;
                if (idx == *current_ws_index_ptr && workspace_host_ptr->count() > 0) return faux;
                auto gear_id_list = decltype(pro::focus::cut(std::declval<sptr>())){};
                auto has_gears = faux;
                if (workspace_host_ptr->count() > 0)
                {
                    auto cur = workspace_host_ptr->back();
                    gear_id_list = pro::focus::cut(cur);
                    has_gears = true;
                    cur->base::detach();
                }
                *previous_ws_index_ptr = *current_ws_index_ptr;
                *current_ws_index_ptr = idx;
                auto target = (*workspaces_ptr)[idx];
                workspace_host_ptr->attach(target);
                target->base::broadcast(tier::anycast, e2::form::upon::started);
                if (has_gears) pro::focus::set(target, gear_id_list, solo::on);
                else           pro::focus::set(target, id_t{}, solo::on);
                workspace_host_ptr->base::reflow();
                (*refresh_status_bar_fn)();
                // Notify menu items (e.g. workspace label) to refresh. The menu
                // workspace/app items both subscribe to e2::form::prop::any and
                // re-fetch their state via Lua on each broadcast, so a single
                // signal here keeps every dependent label in sync.
                if (auto op = object_shadow.lock())
                {
                    auto label = text(1, char(ws_min_index + idx));
                    op->base::broadcast(tier::release, e2::form::prop::any, label);
                }
                return true;
            };

            // Create a new empty workspace and switch to it. Returns the new index, or max() on failure.
            auto create_workspace = [workspaces_ptr, make_workspace_veer, switch_workspace](text selected_id_override = {}) -> size_t
            {
                if (workspaces_ptr->size() >= ws_max_count) return std::numeric_limits<size_t>::max();
                auto new_ws = make_workspace_veer(view{}, selected_id_override);
                if (!selected_id_override.empty())
                {
                    new_ws->base::property("tile.selected") = selected_id_override;
                }
                workspaces_ptr->push_back(new_ws);
                auto new_idx = workspaces_ptr->size() - 1;
                switch_workspace(new_idx);
                return new_idx;
            };

            // Destroy workspace by index. When the last workspace is destroyed, shutdown the tile.
            auto destroy_workspace = [workspaces_ptr, current_ws_index_ptr, workspace_host_ptr, refresh_status_bar_fn, focus_histories_ptr, object_shadow = ptr::shadow(object)](size_t idx) -> bool
            {
                if (idx >= workspaces_ptr->size()) return faux;
                if (workspaces_ptr->size() == 1)
                {
                    close_tile_session(*workspace_host_ptr, "Shutdown on last workspace destroyed");
                    return true;
                }
                auto is_current = (idx == *current_ws_index_ptr);
                auto victim = (*workspaces_ptr)[idx];
                auto gear_id_list = decltype(pro::focus::cut(std::declval<sptr>())){};
                auto has_gears = faux;
                if (is_current && workspace_host_ptr->count() > 0)
                {
                    gear_id_list = pro::focus::cut(workspace_host_ptr->back());
                    has_gears = true;
                    workspace_host_ptr->pop_back();
                }
                workspaces_ptr->erase(workspaces_ptr->begin() + idx);
                focus_histories_ptr->erase(focus_histories_ptr->begin() + idx);
                if (*current_ws_index_ptr >= workspaces_ptr->size())
                {
                    *current_ws_index_ptr = workspaces_ptr->size() - 1;
                }
                else if (*current_ws_index_ptr > idx)
                {
                    *current_ws_index_ptr -= 1;
                }
                if (is_current)
                {
                    auto target = (*workspaces_ptr)[*current_ws_index_ptr];
                    workspace_host_ptr->attach(target);
                    target->base::broadcast(tier::anycast, e2::form::upon::started);
                    if (has_gears) pro::focus::set(target, gear_id_list, solo::on);
                    else           pro::focus::set(target, id_t{}, solo::on);
                    workspace_host_ptr->base::reflow();
                }
                (*refresh_status_bar_fn)();
                if (auto op = object_shadow.lock())
                {
                    auto label = text(1, char(ws_min_index + *current_ws_index_ptr));
                    op->base::broadcast(tier::release, e2::form::prop::any, label);
                }
                // Destroy victim asynchronously outside the auth lock.
                // The keyboard event handler holds the auth lock (recursive_mutex),
                // and ~vtty()::payoff() calls stdinput.join() which can deadlock or
                // cause process exit if done under the lock. Using enqueue<faux>
                // defers destruction to the jobs worker thread with the lock released,
                // matching the pattern used by term::close() and dtvt::stop().
                workspace_host_ptr->base::enqueue<faux>([victim_to_destroy = std::move(victim)](auto& /*boss*/) mutable
                {
                    victim_to_destroy.reset();
                });
                return true;
            };

            // Switch to the next workspace (wrapping around to 0 at the end).
            auto next_workspace = [workspaces_ptr, current_ws_index_ptr, switch_workspace]() -> bool
            {
                if (workspaces_ptr->size() <= 1) return faux;
                auto next_idx = (*current_ws_index_ptr + 1) % workspaces_ptr->size();
                return switch_workspace(next_idx);
            };

            // Switch to the previous workspace (wrapping around to the last at the beginning).
            auto prev_workspace = [workspaces_ptr, current_ws_index_ptr, switch_workspace]() -> bool
            {
                if (workspaces_ptr->size() <= 1) return faux;
                auto prev_idx = (*current_ws_index_ptr == 0) ? workspaces_ptr->size() - 1
                                                             : *current_ws_index_ptr - 1;
                return switch_workspace(prev_idx);
            };

            // Switch to the last visited workspace.
            auto last_workspace = [workspaces_ptr, previous_ws_index_ptr, switch_workspace]() -> bool
            {
                if (workspaces_ptr->size() <= 1) return faux;
                auto prev_idx = *previous_ws_index_ptr;
                if (prev_idx >= workspaces_ptr->size()) prev_idx = workspaces_ptr->size() - 1;
                return switch_workspace(prev_idx);
            };

            // Workspace preview popup state (Win+Tab style; opened from the menu workspace button
            // and from the `vtm.tile.OpenWorkspacePopup()` Lua method).
            auto ws_popup_active = ptr::shared(faux);    // Whether the workspace preview popup is currently open.
            *refresh_status_bar_fn = []
            {
                // No-op: status bar removed; workspace and app-picker controls live in the tile menu now.
                // The lambda is preserved (and called from switch/create/destroy paths) to avoid
                // touching every capture site; the tile menu refreshes its labels via
                // e2::form::prop::any broadcasts emitted by switch/create/destroy and by setapp.
            };

            // Helper: recursively collect pane layout from a workspace veer tree into a flat list with computed rects.
            auto collect_ws_panes_fn = [](auto& self, sptr veer_ptr, rect area, std::vector<ws_thumb_pane_t>& panes, si32 h_par = 0, si32 v_par = 0) -> void
            {
                auto veer = std::dynamic_pointer_cast<ui::veer>(veer_ptr);
                if (!veer || veer->count() == 0) return;
                auto item = veer->back();
                if (!item) return;
                auto cidx = h_par * 2 + v_par;

                if (veer->count() == 1) // Only empty slot.
                {
                    panes.push_back({ area, "~"s, cidx, veer_ptr });
                    return;
                }
                if (item->root()) // App window or empty slot placeholder.
                {
                    auto lbl = text{};
                    if (item->kind() != base::placeholder)
                    {
                        lbl = item->base::signal(tier::request, e2::form::prop::ui::header);
                    }
                    if (lbl.empty()) lbl = "~";
                    panes.push_back({ area, lbl, cidx, veer_ptr });
                    return;
                }
                // Node (split): recurse into children using actual split ratio.
                auto fork_ptr = std::static_pointer_cast<ui::fork>(item);
                auto [orientation, griparea_unused, fraction] = fork_ptr->get_config();
                auto child1 = fork_ptr->get(slot::_1);
                auto child2 = fork_ptr->get(slot::_2);
                static constexpr auto max_ratio = si32{ 0xFFFF };
                if (orientation == axis::X) // Horizontal: children side by side.
                {
                    auto s1 = std::clamp(area.size.x * fraction / max_ratio, 1, area.size.x - 1);
                    auto s2 = area.size.x - s1;
                    auto a1 = rect{ area.coor, { s1, area.size.y } };
                    auto a2 = rect{ { area.coor.x + s1, area.coor.y }, { s2, area.size.y } };
                    self(self, child1, a1, panes, h_par, v_par);
                    self(self, child2, a2, panes, h_par, v_par ^ 1); // Flip v for right child.
                }
                else // Vertical: children stacked.
                {
                    auto s1 = std::clamp(area.size.y * fraction / max_ratio, 1, area.size.y - 1);
                    auto s2 = area.size.y - s1;
                    auto a1 = rect{ area.coor, { area.size.x, s1 } };
                    auto a2 = rect{ { area.coor.x, area.coor.y + s1 }, { area.size.x, s2 } };
                    self(self, child1, a1, panes, h_par, v_par);
                    self(self, child2, a2, panes, h_par ^ 1, v_par); // Flip h for bottom child.
                }
            };

            // Popup layout constants.
            static constexpr auto popup_ws_thumb_ratio_w = si32{ 5 };   // Thumbnail aspect ratio width.
            static constexpr auto popup_ws_thumb_ratio_h = si32{ 2 };   // Thumbnail aspect ratio height.
            static constexpr auto popup_ws_thumb_gap  = si32{ 2 };  // Gap between thumbnails.
             static constexpr auto popup_bottom_pad_y = si32{ 1 };   // Top padding in bottom bar.
             static constexpr auto popup_scrollbar_h  = si32{ 1 };   // Scrollbar row height (always reserved).
            // Popup color palette (Tokyo Night).
            static constexpr auto popup_bar_bg    = (argb)0xff16161e;
            static constexpr auto popup_dim_fg    = (argb)0xff565f89;
            static constexpr auto popup_hov_bg    = (argb)0xff1f2335;
            static constexpr auto popup_hov_fg    = (argb)0xff9aa5ce;
            static constexpr auto popup_hov_ul    = (argb)0xff3b4261;
            static constexpr auto popup_act_bg    = (argb)0xff292e42;
            static constexpr auto popup_act_fg    = (argb)0xffc0caf5;
            static constexpr auto popup_act_ul    = (argb)0xff7aa2f7;
            static constexpr auto popup_thumb_bg  = (argb)0xff1a1b26;  // Thumbnail background.
            static constexpr auto popup_thumb_brd = (argb)0xff3b4261;  // Thumbnail border.
            // Pane fill colors (4-color palette for adjacency-aware coloring).
            static constexpr argb popup_pane_colors[] = {
                // (argb)0xff292e42,  // [0] h=0,v=0 - medium blue-gray.
                // (argb)0xff1a1b26,  // [1] h=0,v=1 - darkest.
                // (argb)0xff2f354b,  // [2] h=1,v=0 - lighter blue-gray.
                // (argb)0xff1f2335,  // [3] h=1,v=1 - dark indigo.
                (argb)0xff1d1f2a,  // [0] h=0,v=0 - medium blue-gray.
                (argb)0xff14151d,  // [1] h=0,v=1 - darkest.
                (argb)0xff232532,  // [2] h=1,v=0 - lighter blue-gray.
                (argb)0xff181a23,  // [3] h=1,v=1 - dark indigo.
            };
            static constexpr auto popup_pane_fg   = (argb)0xff565f89;  // Pane label text.
            static constexpr auto popup_sel_brd   = (argb)0xff7aa2f7;  // Selected/hovered workspace border.
            static constexpr auto popup_idle_brd  = (argb)0xffe0af68;  // "Unfocused but selected" border (yellow accent): used to hint nav state transition.
            // Scrollbar color palette (three states: normal, hover, drag).
            static constexpr auto popup_sb_normal = (argb)0xff3b4261;  // Scrollbar normal fg (subtle).
            static constexpr auto popup_sb_hover  = (argb)0xff565f89;  // Scrollbar hover fg (brighter).
            static constexpr auto popup_sb_drag   = (argb)0xff7aa2f7;  // Scrollbar drag fg (accent blue).
            // Unicode box-drawing border characters (configurable).
            static constexpr auto box_tl = "╭"; // Top-left corner.
            static constexpr auto box_tr = "╮"; // Top-right corner.
            static constexpr auto box_bl = "╰"; // Bottom-left corner.
            static constexpr auto box_br = "╯"; // Bottom-right corner.
            static constexpr auto box_hz = "─"; // Horizontal bar.
            static constexpr auto box_vt = "│"; // Vertical bar.
            // Horizontal scrollbar characters.
            static constexpr auto sb_arrow_l = "◀"; // Left arrow.
            static constexpr auto sb_arrow_r = "▶"; // Right arrow.
            static constexpr auto sb_thumb   = "▬"; // Scrollbar thumb (black rectangle).

            // Helper: draw a Unicode line-frame box and return the interior rect.
            auto draw_popup_box = [](auto& canvas, rect box, argb brd, argb bg, argb fg, auto link) -> rect
            {
                if (box.size.x < 2 || box.size.y < 2)
                {
                    canvas.fill(box, [=](cell& c)
                    {
                        c.bgc(bg).fgc(fg).txt(whitespace).link(link);
                    });
                    return box;
                }
                auto inner = rect{{ box.coor.x + 1, box.coor.y + 1 },
                                   { box.size.x - 2, box.size.y - 2 }};
                if (inner.size.x > 0 && inner.size.y > 0)
                {
                    canvas.fill(inner, [=](cell& c)
                    {
                        c.bgc(bg).fgc(fg).txt(whitespace).link(link);
                    });
                }
                // Top row: ╭───╮
                canvas.fill(rect{{ box.coor.x, box.coor.y }, { 1, 1 }}, [=](cell& c)
                {
                    c.bgc(bg).fgc(brd).txt(text(box_tl)).link(link);
                });
                if (box.size.x > 2)
                {
                    canvas.fill(rect{{ box.coor.x + 1, box.coor.y }, { box.size.x - 2, 1 }}, [=](cell& c)
                    {
                        c.bgc(bg).fgc(brd).txt(text(box_hz)).link(link);
                    });
                }
                canvas.fill(rect{{ box.coor.x + box.size.x - 1, box.coor.y }, { 1, 1 }}, [=](cell& c)
                {
                    c.bgc(bg).fgc(brd).txt(text(box_tr)).link(link);
                });
                // Bottom row: ╰───╯
                canvas.fill(rect{{ box.coor.x, box.coor.y + box.size.y - 1 }, { 1, 1 }}, [=](cell& c)
                {
                    c.bgc(bg).fgc(brd).txt(text(box_bl)).link(link);
                });
                if (box.size.x > 2)
                {
                    canvas.fill(rect{{ box.coor.x + 1, box.coor.y + box.size.y - 1 }, { box.size.x - 2, 1 }}, [=](cell& c)
                    {
                        c.bgc(bg).fgc(brd).txt(text(box_hz)).link(link);
                    });
                }
                canvas.fill(rect{{ box.coor.x + box.size.x - 1, box.coor.y + box.size.y - 1 }, { 1, 1 }}, [=](cell& c)
                {
                    c.bgc(bg).fgc(brd).txt(text(box_br)).link(link);
                });
                // Left and right vertical bars.
                if (box.size.y > 2)
                {
                    canvas.fill(rect{{ box.coor.x, box.coor.y + 1 }, { 1, box.size.y - 2 }}, [=](cell& c)
                    {
                        c.bgc(bg).fgc(brd).txt(text(box_vt)).link(link);
                    });
                    canvas.fill(rect{{ box.coor.x + box.size.x - 1, box.coor.y + 1 }, { 1, box.size.y - 2 }}, [=](cell& c)
                    {
                        c.bgc(bg).fgc(brd).txt(text(box_vt)).link(link);
                    });
                }
                return inner;
            };

            auto wrapper_shadow = ptr::shadow(wrapper);
                // Opens the workspace preview popup (Win+Tab style). Invoked by the menu workspace button
                // and by the `vtm.tile.OpenWorkspacePopup()` Lua method.
                *open_workspace_popup_fn =
                    [workspaces_ptr, current_ws_index_ptr, switch_workspace, create_workspace,
                     ws_popup_active, wrapper_shadow, refresh_status_bar_fn, collect_ws_panes_fn, draw_popup_box]
                {
                    if (*ws_popup_active) return;
                    auto wrapper_ptr = wrapper_shadow.lock();
                    if (!wrapper_ptr) return;

                    *ws_popup_active = true;

                    // Popup shared state.
                    auto preview_idx_ptr  = ptr::shared(*current_ws_index_ptr); // Which workspace is previewed (also keyboard selection in bottom section).
                    auto scroll_off_ptr   = ptr::shared(si32{ -1 });            // Horizontal scroll offset (pixels); -1 = auto-center on first render.
                    auto hover_ws_ptr     = ptr::shared(si32{ -1 });            // Hovered workspace in bottom bar (-1 = none).
                    auto hover_pane_ptr   = ptr::shared(si32{ -1 });            // Mouse-hovered pane in top section (-1 = none).
                    auto hover_sb_ptr     = ptr::shared(faux);                  // Mouse hovering over scrollbar row.
                    auto dragging_sb_ptr  = ptr::shared(faux);                  // Currently dragging scrollbar thumb.
                    auto drag_sb_grab_ptr = ptr::shared(si32{ 0 });             // Grab offset: mouse x - thumb left edge at drag start.
                    // Keyboard navigation state.
                    // focus_section: 0 = top section (pane thumbnails), 1 = bottom section (workspace switcher).
                    // Tab toggles between sections; arrow keys then navigate within the focused section.
                    auto focus_section_ptr = ptr::shared(si32{ 1 });            // Start focused on bottom (workspace switcher).
                    auto kbd_pane_idx_ptr  = ptr::shared(si32{ -1 });           // Keyboard-selected pane in top section (-1 = none).
                    // Keyboard/mouse priority lock: captured mouse coord at the moment of the
                    // last keyboard action. While MouseMove reports the same coord, hover-derived
                    // side effects (updating hover_*, swapping preview_idx) are suppressed so a
                    // stationary cursor cannot override keyboard navigation. Initialized to an
                    // out-of-range sentinel so the first real MouseMove is always honored.
                    auto kbd_lock_coord_ptr = ptr::shared(twod{ -32768, -32768 });
                    // Initialization gate: the very first MouseMove event after the popup opens is
                    // silently discarded and its coord is saved into kbd_lock_coord_ptr. This
                    // prevents a cursor that was already resting on a tab from immediately
                    // highlighting it when the popup appears (e.g. after opening via keyboard
                    // shortcut). Once any genuine mouse movement occurs, this gate is open and
                    // normal hover processing resumes.
                    auto popup_ready_ptr = ptr::shared(faux);

                    // Build the overlay (attached to the wrapper cake).
                    auto overlay_ptr = ui::mock::ctor();
                    auto overlay_shadow = ptr::shadow(overlay_ptr);
                    auto kbd_hook = ptr::shared<hook>();
                    auto pending_unhook = ptr::shared(faux);

                    auto dismiss_visual = [overlay_shadow, ws_popup_active, pending_unhook]
                    {
                        if (*pending_unhook) return;
                        *pending_unhook = true;
                        *ws_popup_active = faux;
                        if (auto p = overlay_shadow.lock()) p->base::detach();
                    };
                    auto dismiss_hook = [kbd_hook]{ kbd_hook->reset(); };

                    overlay_ptr->invoke([&](auto& ovl)
                    {
                        auto ovl_id = ovl.bell::id;

                        // Render callback: draws the full workspace preview popup.
                        ovl.LISTEN(tier::release, e2::render::any, parent_canvas, -,
                            (workspaces_ptr, current_ws_index_ptr, preview_idx_ptr, scroll_off_ptr,
                             hover_ws_ptr, hover_pane_ptr, hover_sb_ptr, dragging_sb_ptr,
                             focus_section_ptr, kbd_pane_idx_ptr,
                             ovl_id, collect_ws_panes_fn, draw_popup_box))
                        {
                            auto canvas_area = parent_canvas.area();
                            auto full_w = canvas_area.size.x;
                            auto full_h = canvas_area.size.y;
                            if (full_w < 4 || full_h < 4) return;

                            // --- Dim the entire tile area (faint overlay). ---
                            parent_canvas.fill([ovl_id](cell& c)
                            {
                                c.bgc().faint();
                                c.fgc().faint();
                                c.cur(text_cursor::none); // Suppress any terminal cursor bleeding through.
                                c.link(ovl_id);
                            });

                            // --- Layout: bottom bar and top section (3:1 ratio). ---
                            auto bot_h = std::max(4, full_h / 4) | 1; // Ensure odd so thumb_h (bot_h - 2) is also odd.
                            auto top_h = full_h - bot_h;
                            auto bot_y = full_h - bot_h;

                            // --- Bottom section: workspace switcher. ---
                            // Background fill for bottom bar.
                            parent_canvas.fill(rect{{ 0, bot_y }, { full_w, bot_h }}, [ovl_id](cell& c)
                            {
                                c.bgc(popup_bar_bg).fgc(popup_bar_bg).txt(whitespace).link(ovl_id);
                            });

                             auto thumb_h = bot_h - popup_bottom_pad_y - popup_scrollbar_h;
                             if (thumb_h < 3) thumb_h = 3;
                             auto thumb_w = std::max(5, thumb_h * popup_ws_thumb_ratio_w / popup_ws_thumb_ratio_h) | 1; // Ensure odd width for centered cross.
                             auto plus_w  = thumb_w;
                             auto ws_count = (si32)workspaces_ptr->size();
                             auto thumb_stride = thumb_w + popup_ws_thumb_gap;
                             // Total width of all workspace thumbs + "+" button.
                             auto has_plus = (workspaces_ptr->size() < ws_max_count);
                             auto total_content_w = ws_count * thumb_stride + (has_plus ? plus_w + popup_ws_thumb_gap : 0);
                             // Auto-center on the current workspace thumbnail on first render.
                            if (*scroll_off_ptr < 0 && total_content_w > full_w)
                            {
                                auto cur_idx = (si32)*current_ws_index_ptr;
                                auto thumb_center = cur_idx * thumb_stride + thumb_w / 2;
                                auto max_scroll = std::max(0, total_content_w - full_w);
                                *scroll_off_ptr = std::clamp(thumb_center - full_w / 2, 0, max_scroll);
                            }
                            else if (*scroll_off_ptr < 0)
                            {
                                *scroll_off_ptr = 0;
                            }
                            // Center the thumbnails if they fit; otherwise allow scrolling.
                            auto scroll = *scroll_off_ptr;
                            auto base_x = (total_content_w <= full_w) ? (full_w - total_content_w) / 2
                                                                      : -scroll;
                            auto thumb_y = bot_y + popup_bottom_pad_y;
                            auto scrollbar_y = thumb_y + thumb_h; // Row reserved for horizontal scrollbar.

                            auto hover_ws = *hover_ws_ptr;
                            auto current_idx = *current_ws_index_ptr;
                            auto preview_idx = *preview_idx_ptr;

                            // Draw each workspace thumbnail.
                            for (auto i = si32{}; i < ws_count; i++)
                            {
                                auto tx = base_x + i * thumb_stride;
                                if (tx + thumb_w < 0 || tx >= full_w) continue; // Off-screen.

                                auto is_current = ((size_t)i == current_idx);
                                auto is_preview = ((size_t)i == preview_idx);
                                auto is_hover   = (i == hover_ws);
                                // Border coloring rule:
                                //  - previewed (keyboard selection) tab:
                                //      * blue (popup_sel_brd) when the bottom section has keyboard focus,
                                //      * yellow (popup_idle_brd) when focus moved to the top section (hint
                                //        that the bottom selection is preserved but inactive).
                                //  - plain mouse-hovered tab (not same as preview): faint underline.
                                //  - otherwise: neutral thumbnail border.
                                auto bot_focused = (*focus_section_ptr == 1);
                                auto brd = is_preview ? (bot_focused ? popup_sel_brd : popup_idle_brd)
                                         : (is_hover && !is_preview) ? popup_hov_ul
                                         :                              popup_thumb_brd;
                                auto tbg = is_current ? popup_act_bg : popup_thumb_bg;

                                // Thumbnail with line-frame border.
                                auto tr = rect{{ tx, thumb_y }, { thumb_w, thumb_h }};
                                auto inner = draw_popup_box(parent_canvas, tr, brd, tbg, popup_pane_fg, ovl_id);
                                if (inner.size.x < 1 || inner.size.y < 1) continue;

                                // Render workspace layout within the inner area (color blocks only, no labels).
                                auto ws_veer = (*workspaces_ptr)[i];
                                auto panes = std::vector<ws_thumb_pane_t>{};
                                collect_ws_panes_fn(collect_ws_panes_fn, std::static_pointer_cast<ui::base>(ws_veer), inner, panes);
                                for (auto& pane : panes)
                                {
                                    auto pr = pane.area;
                                    auto pbg = popup_pane_colors[pane.color_idx & 3];
                                    parent_canvas.fill(pr, [=](cell& c)
                                    {
                                        c.bgc(pbg).fgc(popup_pane_fg).txt(whitespace).link(ovl_id);
                                    });
                                }

                                // Workspace index label at bottom of thumbnail.
                                auto idx_label = text(1, char(ws_min_index + i));
                                auto lx = tx + thumb_w / 2;
                                auto ly = thumb_y + thumb_h - 1;
                                auto lfg = is_current ? popup_act_fg : popup_dim_fg;
                                parent_canvas.fill(rect{{ lx, ly }, { 1, 1 }}, [=](cell& c)
                                {
                                    c.bgc(tbg).fgc(lfg).txt(idx_label).link(ovl_id).bld(is_current);
                                });
                            }

                            // Draw "+" button after the last workspace thumbnail.
                            if (has_plus)
                            {
                                auto px = base_x + ws_count * thumb_stride;
                                if (px < full_w && px + plus_w > 0)
                                {
                                    auto is_hover = (hover_ws == ws_count);
                                    auto pbrd = is_hover ? popup_hov_ul : popup_thumb_brd;
                                    auto pfg  = is_hover ? popup_hov_fg : popup_dim_fg;
                                    auto pr = rect{{ px, thumb_y }, { plus_w, thumb_h }};
                                    auto pin = draw_popup_box(parent_canvas, pr, pbrd, popup_thumb_bg, pfg, ovl_id);
                                    if (pin.size.x >= 1 && pin.size.y >= 1)
                                    {
                                        // Draw a cross using box-drawing characters, occupying half the tab height.
                                        auto cross_h = std::max(si32{1}, pin.size.y / 2);
                                        auto arm_v   = std::max(si32{1}, (cross_h - 1) / 2); // virtical arm at least 1.
                                        auto arm_h   = std::max(si32{1}, arm_v * 2);         // Double horizontal arm to compensate cell aspect ratio ~2:1; at least 1.
                                        auto cy = pin.coor.y + pin.size.y / 2;
                                        auto cx = pin.coor.x + pin.size.x / 2;
                                        // Vertical segments (│).
                                        for (auto dy = -arm_v; dy <= arm_v; dy++)
                                        {
                                            if (dy == 0) continue;
                                            auto py = cy + dy;
                                            if (py >= pin.coor.y && py < pin.coor.y + pin.size.y)
                                            {
                                                parent_canvas.fill(rect{{ cx, py }, { 1, 1 }}, [=](cell& c)
                                                {
                                                    c.bgc(popup_thumb_bg).fgc(pfg).txt("│").link(ovl_id);
                                                });
                                            }
                                        }
                                        // Horizontal segments (─).
                                        for (auto dx = -arm_h; dx <= arm_h; dx++)
                                        {
                                            if (dx == 0) continue;
                                            auto px2 = cx + dx;
                                            if (px2 >= pin.coor.x && px2 < pin.coor.x + pin.size.x)
                                            {
                                                parent_canvas.fill(rect{{ px2, cy }, { 1, 1 }}, [=](cell& c)
                                                {
                                                    c.bgc(popup_thumb_bg).fgc(pfg).txt("─").link(ovl_id);
                                                });
                                            }
                                        }
                                        // Center intersection (┼).
                                        parent_canvas.fill(rect{{ cx, cy }, { 1, 1 }}, [=](cell& c)
                                        {
                                            c.bgc(popup_thumb_bg).fgc(pfg).txt("┼").link(ovl_id);
                                        });
                                    }
                                }
                            }

                            // --- Horizontal scrollbar (rendered only when content overflows). ---
                            if (total_content_w > full_w && full_w >= 3)
                            {
                                auto max_scroll = std::max(1, total_content_w - full_w);
                                auto track_w = full_w - 2; // Between the two arrow cells.
                                auto sb_w = std::max(1, track_w * full_w / total_content_w);
                                auto sb_x = 1 + (scroll * (track_w - sb_w) / max_scroll);
                                auto sb_fg = *dragging_sb_ptr ? popup_sb_drag
                                           : *hover_sb_ptr    ? popup_sb_hover
                                           :                     popup_sb_normal;
                                // Left arrow.
                                parent_canvas.fill(rect{{ 0, scrollbar_y }, { 1, 1 }}, [=](cell& c)
                                {
                                    c.bgc(popup_bar_bg).fgc(sb_fg).txt(text(sb_arrow_l)).link(ovl_id);
                                });
                                // Right arrow.
                                parent_canvas.fill(rect{{ full_w - 1, scrollbar_y }, { 1, 1 }}, [=](cell& c)
                                {
                                    c.bgc(popup_bar_bg).fgc(sb_fg).txt(text(sb_arrow_r)).link(ovl_id);
                                });
                                // Thumb (full block).
                                for (auto sx = si32{}; sx < sb_w; sx++)
                                {
                                    parent_canvas.fill(rect{{ sb_x + sx, scrollbar_y }, { 1, 1 }}, [=](cell& c)
                                    {
                                        c.bgc(popup_bar_bg).fgc(sb_fg).txt(text(sb_thumb)).link(ovl_id);
                                    });
                                }
                            }

                            // --- Top section: pane thumbnails for the previewed workspace. ---
                            auto prev_idx = *preview_idx_ptr;
                            if (prev_idx < workspaces_ptr->size())
                            {
                                auto ws_veer = (*workspaces_ptr)[prev_idx];
                                auto top_area = rect{{ 0, 0 }, { full_w, top_h }};
                                if (top_area.size.x >= 1 && top_area.size.y >= 1)
                                {
                                    auto panes = std::vector<ws_thumb_pane_t>{};
                                    collect_ws_panes_fn(collect_ws_panes_fn, std::static_pointer_cast<ui::base>(ws_veer), top_area, panes);
                                    auto kp = *kbd_pane_idx_ptr;
                                    auto top_focused = (*focus_section_ptr == 0);
                                    auto pidx = si32{};
                                    for (auto& pane : panes)
                                    {
                                        auto pr = pane.area;
                                        // Unified active-pane cursor: kbd_pane_idx drives the highlight.
                                        // The mouse updates that same index from MouseMove, so both
                                        // inputs share one indicator and the visuals never disagree.
                                        auto is_active = top_focused && (pidx == kp);
                                        auto pbg  = is_active ? popup_act_bg : popup_pane_colors[pane.color_idx & 3];
                                        auto pfg  = is_active ? popup_act_fg : popup_pane_fg;
                                        auto pbrd = is_active ? popup_sel_brd : popup_thumb_brd;

                                        // Line-frame border.
                                        auto pin = draw_popup_box(parent_canvas, pr, pbrd, pbg, pfg, ovl_id);
                                        // Pane label centered in the card.
                                        if (pin.size.x >= 1 && pin.size.y >= 1)
                                        {
                                            auto max_lw = pin.size.x;
                                            auto short_label = pane.label.substr(0, std::min((si32)pane.label.size(), max_lw));
                                            auto lx = pin.coor.x + (pin.size.x - (si32)short_label.size()) / 2;
                                            auto ly = pin.coor.y + pin.size.y / 2;
                                            for (auto ci = si32{}; ci < (si32)short_label.size(); ci++)
                                            {
                                                parent_canvas.fill(rect{{ lx + ci, ly }, { 1, 1 }}, [=, ch = text(1, short_label[ci])](cell& c)
                                                {
                                                    c.bgc(pbg).fgc(pfg).txt(ch).link(ovl_id);
                                                });
                                            }
                                        }
                                        // Pane index badge in top-left corner.
                                        auto badge = text(1, char(ws_min_index + pidx));
                                        parent_canvas.fill(rect{{ pr.coor.x + 1, pr.coor.y }, { 1, 1 }}, [=](cell& c)
                                        {
                                            c.bgc(pbg).fgc(pfg).txt(badge).link(ovl_id);
                                        });
                                        pidx++;
                                    }
                                }
                            }
                        };

                        // --- Mouse event handlers for the popup. ---
                        // Compute hit areas and dispatch mouse actions.
                        ovl.on(tier::mouserelease, input::key::MouseMove,
                            [workspaces_ptr, current_ws_index_ptr, preview_idx_ptr, scroll_off_ptr,
                             hover_ws_ptr, hover_pane_ptr, hover_sb_ptr,
                             kbd_pane_idx_ptr, kbd_lock_coord_ptr, focus_section_ptr,
                             collect_ws_panes_fn, overlay_shadow, popup_ready_ptr](hids& gear)
                        {
                            // Initialization gate: discard the very first MouseMove (which reflects
                            // the cursor position before the popup opened) so that a pre-resting
                            // cursor never highlights a tab on popup entry. Lock kbd_lock_coord_ptr
                            // to that coord so a stationary cursor also stays suppressed.
                            if (!*popup_ready_ptr)
                            {
                                *popup_ready_ptr = true;
                                *kbd_lock_coord_ptr = gear.coord;
                                return;
                            }
                            auto ovl_ptr = overlay_shadow.lock();
                            if (!ovl_ptr) return;
                            auto full_area = ovl_ptr->base::area();
                            auto full_w = full_area.size.x;
                            auto full_h = full_area.size.y;
                            auto mx = gear.coord.x;
                            auto my = gear.coord.y;
                            // Keyboard priority: while the mouse cursor remains at the exact cell
                            // it occupied when the last keyboard action fired, treat this MouseMove
                            // as noise and skip all hover-derived updates. As soon as the cursor
                            // lands on a different cell we drop the lock and process normally.
                            if (gear.coord == *kbd_lock_coord_ptr) return;
                            *kbd_lock_coord_ptr = twod{ -32768, -32768 };
                            auto bot_h = std::max(4, full_h / 4) | 1; // Ensure odd so thumb_h (bot_h - 2) is also odd.
                            auto top_h = full_h - bot_h;
                            auto bot_y = full_h - bot_h;

                            auto old_hover_ws = *hover_ws_ptr;
                            auto old_hover_pane = *hover_pane_ptr;
                            auto old_hover_sb = *hover_sb_ptr;
                            auto new_hover_ws = si32{ -1 };
                            auto new_hover_pane = si32{ -1 };
                            auto new_hover_sb = faux;

                            if (my >= bot_y) // In bottom bar.
                            {
                                auto ws_count = (si32)workspaces_ptr->size();
                                auto has_plus = (workspaces_ptr->size() < ws_max_count);
                                auto thumb_h = bot_h - popup_bottom_pad_y - popup_scrollbar_h;
                                if (thumb_h < 3) thumb_h = 3;
                                auto thumb_w = std::max(5, thumb_h * popup_ws_thumb_ratio_w / popup_ws_thumb_ratio_h) | 1; // Ensure odd width for centered cross.
                                auto plus_w  = thumb_w;
                                auto thumb_stride = thumb_w + popup_ws_thumb_gap;
                                auto total_content_w = ws_count * thumb_stride + (has_plus ? plus_w + popup_ws_thumb_gap : 0);
                                auto scroll = *scroll_off_ptr;
                                auto base_x = (total_content_w <= full_w) ? (full_w - total_content_w) / 2 : -scroll;
                                auto thumb_y = bot_y + popup_bottom_pad_y;
                                auto scrollbar_y = thumb_y + thumb_h;

                                if (my >= thumb_y && my < thumb_y + thumb_h)
                                {
                                    // Check workspace thumbnails.
                                    for (auto i = si32{}; i < ws_count; i++)
                                    {
                                        auto tx = base_x + i * thumb_stride;
                                        if (mx >= tx && mx < tx + thumb_w)
                                        {
                                            new_hover_ws = i;
                                            break;
                                        }
                                    }
                                    // Check "+" button.
                                    if (new_hover_ws < 0 && has_plus)
                                    {
                                        auto px = base_x + ws_count * thumb_stride;
                                        if (mx >= px && mx < px + plus_w)
                                        {
                                            new_hover_ws = ws_count; // "+" is at index == ws_count.
                                        }
                                    }
                                }
                                else if (my == scrollbar_y && total_content_w > full_w)
                                {
                                    new_hover_sb = true;
                                }
                            }
                            else if (my < top_h && my >= 0) // In top section.
                            {
                                auto prev_idx = *preview_idx_ptr;
                                if (prev_idx < workspaces_ptr->size())
                                {
                                    auto ws_veer = (*workspaces_ptr)[prev_idx];
                                    auto top_area = rect{{ 0, 0 }, { full_w, top_h }};
                                    auto panes = std::vector<ws_thumb_pane_t>{};
                                    collect_ws_panes_fn(collect_ws_panes_fn, std::static_pointer_cast<ui::base>(ws_veer), top_area, panes);
                                    auto pidx = si32{};
                                    for (auto& pane : panes)
                                    {
                                        auto pr = pane.area;
                                        if (mx >= pr.coor.x && mx < pr.coor.x + pr.size.x &&
                                            my >= pr.coor.y && my < pr.coor.y + pr.size.y)
                                        {
                                            new_hover_pane = pidx;
                                            break;
                                        }
                                        pidx++;
                                    }
                                }
                            }

                            *hover_ws_ptr = new_hover_ws;
                            *hover_pane_ptr = new_hover_pane;
                            *hover_sb_ptr = new_hover_sb;

                            // Top-section: mouse and keyboard share a single "active pane" cursor.
                            // When the mouse lands on a pane it takes over that cursor (and pulls focus
                            // to the top section). The next arrow key will therefore resume from the
                            // pane the mouse last pointed at. When the mouse moves off all panes we
                            // deliberately leave the cursor where it was so it remains stable.
                            if (new_hover_pane >= 0)
                            {
                                *kbd_pane_idx_ptr = new_hover_pane;
                                *focus_section_ptr = 0;
                            }

                            // Update preview workspace when the mouse genuinely moves to a different
                            // workspace thumbnail. (The kbd-lock guard at the top of this handler
                            // ensures stationary cursors cannot trigger this path.)
                            if (new_hover_ws >= 0 && new_hover_ws < (si32)workspaces_ptr->size()
                                && (size_t)new_hover_ws != *preview_idx_ptr)
                            {
                                *preview_idx_ptr = (size_t)new_hover_ws;
                                *hover_pane_ptr = -1; // Reset pane hover on workspace change.
                                *kbd_pane_idx_ptr = -1; // Reset top keyboard selection since the previewed workspace changed.
                                *focus_section_ptr = 1; // Pull focus to the bottom (workspace switcher).
                            }

                            if (old_hover_ws != new_hover_ws || old_hover_pane != new_hover_pane || old_hover_sb != new_hover_sb)
                            {
                                if (auto p = overlay_shadow.lock()) p->base::deface();
                            }
                        });

                        // Clear hover states when mouse leaves the overlay.
                        ovl.on(tier::mouserelease, input::key::MouseLeave,
                            [hover_ws_ptr, hover_pane_ptr, hover_sb_ptr, overlay_shadow](hids& /*gear*/)
                        {
                            auto changed = (*hover_ws_ptr != -1 || *hover_pane_ptr != -1 || *hover_sb_ptr);
                            *hover_ws_ptr = -1;
                            *hover_pane_ptr = -1;
                            *hover_sb_ptr = faux;
                            if (changed)
                            {
                                if (auto p = overlay_shadow.lock()) p->base::deface();
                            }
                        });

                        // Mouse wheel: scroll the bottom workspace switcher.
                        ovl.on(tier::mouserelease, input::key::MouseWheel,
                            [workspaces_ptr, scroll_off_ptr, overlay_shadow](hids& gear)
                        {
                            auto ovl_ptr = overlay_shadow.lock();
                            if (!ovl_ptr) return;
                            auto full_w = ovl_ptr->base::area().size.x;
                            auto full_h = ovl_ptr->base::area().size.y;
                            auto bot_h = std::max(4, full_h / 4) | 1; // Ensure odd so thumb_h (bot_h - 2) is also odd.
                            auto thumb_h = bot_h - popup_bottom_pad_y - popup_scrollbar_h;
                            if (thumb_h < 3) thumb_h = 3;
                            auto thumb_w = std::max(5, thumb_h * popup_ws_thumb_ratio_w / popup_ws_thumb_ratio_h) | 1; // Ensure odd width for centered cross.
                            auto plus_w  = thumb_w;
                            auto ws_count = (si32)workspaces_ptr->size();
                            auto has_plus = (workspaces_ptr->size() < ws_max_count);
                            auto thumb_stride = thumb_w + popup_ws_thumb_gap;
                            auto total_content_w = ws_count * thumb_stride + (has_plus ? plus_w + popup_ws_thumb_gap : 0);
                            auto max_scroll = std::max(0, total_content_w - full_w);
                            auto delta = -gear.whlsi * 4; // Scroll step.
                            *scroll_off_ptr = std::clamp(*scroll_off_ptr + delta, 0, max_scroll);
                            ovl_ptr->base::deface();
                            gear.dismiss();
                        });

                        // Left click: switch workspace, select pane, or create workspace.
                        ovl.on(tier::mouserelease, input::key::LeftClick,
                            [workspaces_ptr, current_ws_index_ptr, switch_workspace, create_workspace,
                             preview_idx_ptr, scroll_off_ptr, hover_ws_ptr, hover_pane_ptr,
                             dismiss_visual, dismiss_hook, refresh_status_bar_fn, collect_ws_panes_fn,
                             overlay_shadow](hids& gear)
                        {
                            auto ovl_ptr = overlay_shadow.lock();
                            if (!ovl_ptr) { gear.dismiss(); return; }
                            auto full_area = ovl_ptr->base::area();
                            auto full_w = full_area.size.x;
                            auto full_h = full_area.size.y;
                            auto mx = gear.coord.x;
                            auto my = gear.coord.y;
                            auto bot_h = std::max(4, full_h / 4) | 1; // Ensure odd so thumb_h (bot_h - 2) is also odd.
                            auto top_h = full_h - bot_h;
                            auto bot_y = full_h - bot_h;

                            if (my >= bot_y) // Click in bottom bar.
                            {
                                auto ws_count = (si32)workspaces_ptr->size();
                                auto has_plus = (workspaces_ptr->size() < ws_max_count);
                                auto thumb_h = bot_h - popup_bottom_pad_y - popup_scrollbar_h;
                                if (thumb_h < 3) thumb_h = 3;
                                auto thumb_w = std::max(5, thumb_h * popup_ws_thumb_ratio_w / popup_ws_thumb_ratio_h) | 1; // Ensure odd width for centered cross.
                                auto plus_w  = thumb_w;
                                auto thumb_stride = thumb_w + popup_ws_thumb_gap;
                                auto total_content_w = ws_count * thumb_stride + (has_plus ? plus_w + popup_ws_thumb_gap : 0);
                                auto scroll = *scroll_off_ptr;
                                auto base_x = (total_content_w <= full_w) ? (full_w - total_content_w) / 2 : -scroll;
                                auto thumb_y = bot_y + popup_bottom_pad_y;
                                auto scrollbar_y = thumb_y + thumb_h;

                                if (my >= thumb_y && my < thumb_y + thumb_h)
                                {
                                    for (auto i = si32{}; i < ws_count; i++)
                                    {
                                        auto tx = base_x + i * thumb_stride;
                                        if (mx >= tx && mx < tx + thumb_w)
                                        {
                                            // Click on workspace thumbnail: switch to it and dismiss.
                                            dismiss_visual();
                                            dismiss_hook();
                                            switch_workspace((size_t)i);
                                            (*refresh_status_bar_fn)();
                                            gear.dismiss();
                                            return;
                                        }
                                    }
                                    // Check "+" button.
                                    if (has_plus)
                                    {
                                        auto px = base_x + ws_count * thumb_stride;
                                        if (mx >= px && mx < px + plus_w)
                                        {
                                            // Resolve the currently selected app, matching CreateWorkspace
                                            // lua method behavior. The selectapp handler writes
                                            // "tile.selected" on both the outer tile boss (object) and
                                            // the active workspace veer. The overlay is attached to the
                                            // wrapper (cake), so its parent chain does NOT reach `object`
                                            // or the workspace veer; read the property directly from the
                                            // currently active workspace veer instead.
                                            auto selected_override = text{};
                                            if (!workspaces_ptr->empty())
                                            {
                                                auto idx = std::min(*current_ws_index_ptr, workspaces_ptr->size() - 1);
                                                auto& cur_ws = (*workspaces_ptr)[idx];
                                                if (cur_ws)
                                                {
                                                    auto& prop = cur_ws->base::property("tile.selected");
                                                    if (!prop.empty()) selected_override = prop;
                                                }
                                            }
                                            dismiss_visual();
                                            dismiss_hook();
                                            create_workspace(selected_override);
                                            (*refresh_status_bar_fn)();
                                            gear.dismiss();
                                            return;
                                        }
                                    }
                                }
                                else if (my == scrollbar_y && total_content_w > full_w && full_w >= 3)
                                {
                                    // Click on scrollbar row: arrows or track.
                                    auto max_scroll = std::max(1, total_content_w - full_w);
                                    auto track_w = full_w - 2;
                                    auto sb_w = std::max(1, track_w * full_w / total_content_w);

                                    if (mx == 0) // Left arrow: scroll left by one thumbnail stride.
                                    {
                                        *scroll_off_ptr = std::max(0, *scroll_off_ptr - thumb_stride);
                                    }
                                    else if (mx == full_w - 1) // Right arrow: scroll right by one thumbnail stride.
                                    {
                                        *scroll_off_ptr = std::min(max_scroll, *scroll_off_ptr + thumb_stride);
                                    }
                                    else if (mx >= 1 && mx < full_w - 1) // Track area: snap thumb center to click.
                                    {
                                        auto new_sb_x = (si32)mx - sb_w / 2 - 1;
                                        auto new_scroll = (track_w > sb_w) ? new_sb_x * max_scroll / (track_w - sb_w) : 0;
                                        *scroll_off_ptr = std::clamp(new_scroll, 0, max_scroll);
                                    }
                                    if (auto p = overlay_shadow.lock()) p->base::deface();
                                    gear.dismiss();
                                    return;
                                }
                            }
                            else if (my < top_h && my >= 0) // Click in top section.
                            {
                                auto prev_idx = *preview_idx_ptr;
                                if (prev_idx < workspaces_ptr->size())
                                {
                                    auto ws_veer = (*workspaces_ptr)[prev_idx];
                                    auto top_area = rect{{ 0, 0 }, { full_w, top_h }};
                                    auto panes = std::vector<ws_thumb_pane_t>{};
                                    collect_ws_panes_fn(collect_ws_panes_fn, std::static_pointer_cast<ui::base>(ws_veer), top_area, panes);
                                    for (auto& pane : panes)
                                    {
                                        auto pr = pane.area;
                                        if (mx >= pr.coor.x && mx < pr.coor.x + pr.size.x &&
                                            my >= pr.coor.y && my < pr.coor.y + pr.size.y)
                                        {
                                            // Click on a pane: switch to workspace + focus this pane.
                                            dismiss_visual();
                                            dismiss_hook();
                                            switch_workspace(prev_idx);
                                            if (auto focus_target = get_slot_focus_target(pane.slot_veer))
                                            {
                                                pro::focus::set(focus_target, gear.id, solo::on);
                                            }
                                            (*refresh_status_bar_fn)();
                                            gear.dismiss();
                                            return;
                                        }
                                    }
                                }
                            }
                            // Click on empty area: dismiss popup.
                            dismiss_visual();
                            dismiss_hook();
                            gear.dismiss();
                        });

                        // Left drag start: initiate scrollbar thumb drag if click is on the scrollbar row.
                        ovl.on(tier::mouserelease, input::key::LeftDragStart,
                            [workspaces_ptr, scroll_off_ptr, dragging_sb_ptr, hover_sb_ptr,
                             drag_sb_grab_ptr, overlay_shadow](hids& gear)
                        {
                            auto ovl_ptr = overlay_shadow.lock();
                            if (!ovl_ptr) return;
                            auto full_w = ovl_ptr->base::area().size.x;
                            auto full_h = ovl_ptr->base::area().size.y;
                            auto mx = (si32)gear.pressxy.x;
                            auto my = (si32)gear.pressxy.y;
                            auto bot_h = std::max(4, full_h / 4) | 1; // Ensure odd so thumb_h (bot_h - 2) is also odd.
                            auto bot_y = full_h - bot_h;
                            auto thumb_h = bot_h - popup_bottom_pad_y - popup_scrollbar_h;
                            if (thumb_h < 3) thumb_h = 3;
                            auto thumb_w = std::max(5, thumb_h * popup_ws_thumb_ratio_w / popup_ws_thumb_ratio_h) | 1; // Ensure odd width for centered cross.
                            auto plus_w  = thumb_w;
                            auto ws_count = (si32)workspaces_ptr->size();
                            auto has_plus = (workspaces_ptr->size() < ws_max_count);
                            auto thumb_stride = thumb_w + popup_ws_thumb_gap;
                            auto total_content_w = ws_count * thumb_stride + (has_plus ? plus_w + popup_ws_thumb_gap : 0);
                            auto thumb_y = bot_y + popup_bottom_pad_y;
                            auto scrollbar_y = thumb_y + thumb_h;

                            if (my == scrollbar_y && total_content_w > full_w && full_w >= 3)
                            {
                                auto max_scroll = std::max(1, total_content_w - full_w);
                                auto track_w = full_w - 2;
                                auto sb_w = std::max(1, track_w * full_w / total_content_w);
                                auto scroll = *scroll_off_ptr;
                                auto sb_x = 1 + (scroll * (track_w - sb_w) / max_scroll);

                                if (mx >= sb_x && mx < sb_x + sb_w)
                                {
                                    // Click is on the thumb: record grab offset.
                                    *drag_sb_grab_ptr = mx - sb_x;
                                }
                                else
                                {
                                    // Click is on the track but not on the thumb: snap thumb center to click.
                                    *drag_sb_grab_ptr = sb_w / 2;
                                    auto new_sb_x = mx - *drag_sb_grab_ptr - 1;
                                    auto new_scroll = (track_w > sb_w) ? new_sb_x * max_scroll / (track_w - sb_w) : 0;
                                    *scroll_off_ptr = std::clamp(new_scroll, 0, max_scroll);
                                }
                                *dragging_sb_ptr = true;
                                *hover_sb_ptr = true;
                                ovl_ptr->base::deface();
                                gear.dismiss();
                            }
                        });

                        // Left drag pull: update scroll offset while dragging the scrollbar thumb.
                        ovl.on(tier::mouserelease, input::key::LeftDragPull,
                            [workspaces_ptr, scroll_off_ptr, dragging_sb_ptr,
                             drag_sb_grab_ptr, overlay_shadow](hids& gear)
                        {
                            if (!*dragging_sb_ptr) return;
                            auto ovl_ptr = overlay_shadow.lock();
                            if (!ovl_ptr) return;
                            auto full_w = ovl_ptr->base::area().size.x;
                            auto full_h = ovl_ptr->base::area().size.y;
                            auto mx = (si32)gear.coord.x;
                            auto bot_h = std::max(4, full_h / 4) | 1; // Ensure odd so thumb_h (bot_h - 2) is also odd.
                            auto thumb_h = bot_h - popup_bottom_pad_y - popup_scrollbar_h;
                            if (thumb_h < 3) thumb_h = 3;
                            auto thumb_w = std::max(5, thumb_h * popup_ws_thumb_ratio_w / popup_ws_thumb_ratio_h) | 1; // Ensure odd width for centered cross.
                            auto plus_w  = thumb_w;
                            auto ws_count = (si32)workspaces_ptr->size();
                            auto has_plus = (workspaces_ptr->size() < ws_max_count);
                            auto thumb_stride = thumb_w + popup_ws_thumb_gap;
                            auto total_content_w = ws_count * thumb_stride + (has_plus ? plus_w + popup_ws_thumb_gap : 0);
                            auto max_scroll = std::max(1, total_content_w - full_w);
                            auto track_w = full_w - 2;
                            auto sb_w = std::max(1, track_w * full_w / total_content_w);

                            auto new_sb_x = mx - *drag_sb_grab_ptr - 1;
                            auto new_scroll = (track_w > sb_w) ? new_sb_x * max_scroll / (track_w - sb_w) : 0;
                            *scroll_off_ptr = std::clamp(new_scroll, 0, max_scroll);
                            ovl_ptr->base::deface();
                            gear.dismiss();
                        });

                        // Left drag stop: end scrollbar drag.
                        ovl.on(tier::mouserelease, input::key::LeftDragStop,
                            [dragging_sb_ptr, hover_sb_ptr, overlay_shadow](hids& gear)
                        {
                            if (!*dragging_sb_ptr) return;
                            *dragging_sb_ptr = faux;
                            // Check if mouse is still on the scrollbar row to keep hover.
                            if (auto ovl_ptr = overlay_shadow.lock())
                            {
                                auto full_h = ovl_ptr->base::area().size.y;
                                auto my = (si32)gear.coord.y;
                                auto bot_h = std::max(4, full_h / 4) | 1; // Ensure odd so thumb_h (bot_h - 2) is also odd.
                                auto bot_y = full_h - bot_h;
                                auto thumb_h = bot_h - popup_bottom_pad_y - popup_scrollbar_h;
                                if (thumb_h < 3) thumb_h = 3;
                                auto thumb_y = bot_y + popup_bottom_pad_y;
                                auto scrollbar_y = thumb_y + thumb_h;
                                *hover_sb_ptr = (my == scrollbar_y);
                                ovl_ptr->base::deface();
                            }
                            gear.dismiss();
                        });

                        // Left drag cancel: clear scrollbar drag state.
                        ovl.on(tier::mouserelease, input::key::LeftDragCancel,
                            [dragging_sb_ptr, hover_sb_ptr, overlay_shadow](hids& gear)
                        {
                            if (!*dragging_sb_ptr) return;
                            *dragging_sb_ptr = faux;
                            *hover_sb_ptr = faux;
                            if (auto p = overlay_shadow.lock()) p->base::deface();
                            gear.dismiss();
                        });
                    });

                    wrapper_ptr->attach(overlay_ptr);
                    wrapper_ptr->base::reflow();
                    wrapper_ptr->base::deface();

                    // Keyboard interceptor: Esc, Tab, arrow keys, Enter, number keys.
                    auto wrapper_shadow_inner = ptr::shadow(wrapper_ptr);
                    wrapper_ptr->bell::submit(tier::preview, input::events::keybd::any, *kbd_hook)
                        = [dismiss_visual, dismiss_hook, pending_unhook,
                           workspaces_ptr, current_ws_index_ptr, preview_idx_ptr, scroll_off_ptr,
                           focus_section_ptr, kbd_pane_idx_ptr, kbd_lock_coord_ptr, hover_ws_ptr, hover_pane_ptr,
                           switch_workspace, create_workspace, refresh_status_bar_fn,
                           collect_ws_panes_fn, overlay_shadow](hids& gear) mutable
                    {
                        if (gear.payload != input::keybd::type::keypress
                            || gear.keystat == input::key::interrupted
                            || gear.keybd::handled)
                        {
                            return;
                        }
                        if (*pending_unhook)
                        {
                            gear.set_handled(faux);
                            if (gear.keystat == input::key::released) dismiss_hook();
                            return;
                        }
                        if (gear.keystat == input::key::released)
                        {
                            gear.set_handled(faux);
                            return;
                        }
                        auto k = gear.keybd::generic();
                        // Esc dismisses.
                        if (k == input::key::Esc)
                        {
                            dismiss_visual();
                            gear.set_handled(faux);
                            return;
                        }

                        // --- Helpers for top-section pane keyboard navigation. ---
                        // Rebuild the pane list for the currently previewed workspace in the top-section
                        // coordinate space so we can reason about pane geometry the same way the renderer does.
                        auto gather_top_panes = [&]() -> std::vector<ws_thumb_pane_t>
                        {
                            auto panes = std::vector<ws_thumb_pane_t>{};
                            auto ovl_ptr = overlay_shadow.lock();
                            if (!ovl_ptr) return panes;
                            auto full_area = ovl_ptr->base::area();
                            auto full_w = full_area.size.x;
                            auto full_h = full_area.size.y;
                            auto bot_h = std::max(4, full_h / 4) | 1;
                            auto top_h = full_h - bot_h;
                            if (full_w < 1 || top_h < 1) return panes;
                            auto prev_idx = *preview_idx_ptr;
                            if (prev_idx >= workspaces_ptr->size()) return panes;
                            auto ws_veer = (*workspaces_ptr)[prev_idx];
                            auto top_area = rect{{ 0, 0 }, { full_w, top_h }};
                            collect_ws_panes_fn(collect_ws_panes_fn, std::static_pointer_cast<ui::base>(ws_veer), top_area, panes);
                            return panes;
                        };
                        // 2D nearest-neighbour pane navigation. Delegates the per-candidate
                        // scoring to the shared `score_pane_direction` helper, ensuring the popup
                        // mirrors the in-tile `navigate` lambda behaviour.
                        auto pane_navigate = [&](si32 cur_idx, twod dir) -> si32
                        {
                            auto panes = gather_top_panes();
                            if (panes.empty()) return -1;
                            if (cur_idx < 0 || cur_idx >= (si32)panes.size()) return 0;
                            auto src = panes[cur_idx].area;
                            auto best_idx = si32{ -1 };
                            auto best_score = std::tuple<si64, si64, si64>{ si64max, si64max, si64max };
                            for (auto i = si32{}; i < (si32)panes.size(); i++)
                            {
                                if (i == cur_idx) continue;
                                auto score = score_pane_direction(src, panes[i].area, dir);
                                if (score && *score < best_score)
                                {
                                    best_score = *score;
                                    best_idx = i;
                                }
                            }
                            return best_idx;
                        };

                        // --- Tab: toggle keyboard focus between top (0) and bottom (1) sections. ---
                        if (k == input::key::Tab)
                        {
                            if (*focus_section_ptr == 1)
                            {
                                // Bottom -> Top. Preserve the bottom's preview_idx border (drawn yellow
                                // while focus is elsewhere). Seed the top keyboard selection at pane 0
                                // if not already set.
                                *focus_section_ptr = 0;
                                auto panes = gather_top_panes();
                                if (panes.empty()) *kbd_pane_idx_ptr = -1;
                                else if (*kbd_pane_idx_ptr < 0 || *kbd_pane_idx_ptr >= (si32)panes.size())
                                    *kbd_pane_idx_ptr = 0;
                                // Clear mouse hover indicators to keep the single-focus invariant.
                                *hover_ws_ptr = -1;
                                *hover_pane_ptr = -1;
                                *kbd_lock_coord_ptr = gear.coord; // Lock out echo MouseMove at this coord.
                            }
                            else
                            {
                                // Top -> Bottom. Drop the top keyboard selection entirely.
                                *focus_section_ptr = 1;
                                *kbd_pane_idx_ptr = -1;
                                *hover_ws_ptr = -1;
                                *hover_pane_ptr = -1;
                                *kbd_lock_coord_ptr = gear.coord; // Lock out echo MouseMove at this coord.
                            }
                            if (auto p = overlay_shadow.lock()) p->base::deface();
                            gear.set_handled(faux);
                            return;
                        }

                        // Enter: commit the current selection depending on which section owns focus.
                        if (k == input::key::KeyEnter)
                        {
                            auto idx = *preview_idx_ptr;
                            if (*focus_section_ptr == 0) // Top section: focus the keyboard-selected pane.
                            {
                                auto kp = *kbd_pane_idx_ptr;
                                auto panes = gather_top_panes();
                                if (kp >= 0 && kp < (si32)panes.size())
                                {
                                    auto slot_veer = panes[kp].slot_veer;
                                    dismiss_visual();
                                    dismiss_hook();
                                    if (idx < workspaces_ptr->size())
                                    {
                                        switch_workspace(idx);
                                        if (auto focus_target = get_slot_focus_target(slot_veer))
                                        {
                                            pro::focus::set(focus_target, gear.id, solo::on);
                                        }
                                        (*refresh_status_bar_fn)();
                                    }
                                    gear.set_handled(faux);
                                    return;
                                }
                            }
                            // Bottom section (or top section with no pane selected): switch to the previewed ws.
                            dismiss_visual();
                            dismiss_hook();
                            if (idx < workspaces_ptr->size())
                            {
                                switch_workspace(idx);
                                (*refresh_status_bar_fn)();
                            }
                            gear.set_handled(faux);
                            return;
                        }

                        // Arrow keys: dispatch per section.
                        auto ws_count = (si32)workspaces_ptr->size();
                        auto is_arrow = (k == input::key::KeyLeftArrow || k == input::key::KeyRightArrow
                                      || k == input::key::KeyUpArrow   || k == input::key::KeyDownArrow);
                        if (is_arrow)
                        {
                            if (*focus_section_ptr == 0) // Top section: 2D pane navigation across all four directions.
                            {
                                auto panes = gather_top_panes();
                                if (panes.empty())
                                {
                                    gear.set_handled(faux);
                                    return;
                                }
                                auto cur = *kbd_pane_idx_ptr;
                                if (cur < 0 || cur >= (si32)panes.size()) cur = 0;
                                auto dir = twod{ 0, 0 };
                                if (k == input::key::KeyLeftArrow)       dir = twod{ -1,  0 };
                                else if (k == input::key::KeyRightArrow) dir = twod{  1,  0 };
                                else if (k == input::key::KeyUpArrow)    dir = twod{  0, -1 };
                                else                                      dir = twod{  0,  1 };
                                auto next_idx = pane_navigate(cur, dir);
                                if (next_idx >= 0) *kbd_pane_idx_ptr = next_idx;
                                else               *kbd_pane_idx_ptr = cur; // Hit an edge: stay put.
                                // Kbd nav clears mouse hover to keep exclusivity.
                                *hover_pane_ptr = -1;
                                *hover_ws_ptr = -1;
                                *kbd_lock_coord_ptr = gear.coord; // Lock out echo MouseMove at this coord.
                                if (auto p = overlay_shadow.lock()) p->base::deface();
                                gear.set_handled(faux);
                                return;
                            }
                            // Bottom section: only left/right browse workspaces; up/down are swallowed.
                            if (ws_count > 0 && (k == input::key::KeyLeftArrow || k == input::key::KeyRightArrow))
                            {
                                auto cur = (si32)*preview_idx_ptr;
                                if (k == input::key::KeyLeftArrow)
                                    cur = (cur > 0) ? cur - 1 : ws_count - 1;
                                else
                                    cur = (cur < ws_count - 1) ? cur + 1 : 0;
                                *preview_idx_ptr = (size_t)cur;
                                // Bottom selection changed: reset top keyboard selection (pane indices are
                                // tied to a specific workspace) and clear mouse hover for exclusivity.
                                *kbd_pane_idx_ptr = -1;
                                *hover_ws_ptr = -1;
                                *hover_pane_ptr = -1;
                                *kbd_lock_coord_ptr = gear.coord; // Lock out echo MouseMove at this coord.
                                // Auto-scroll bottom bar to keep the previewed thumbnail visible.
                                if (auto ovl_ptr = overlay_shadow.lock())
                                {
                                    auto full_w = ovl_ptr->base::area().size.x;
                                    auto full_h = ovl_ptr->base::area().size.y;
                                    auto bot_h = std::max(4, full_h / 4) | 1; // Ensure odd so thumb_h (bot_h - 2) is also odd.
                                    auto thumb_h = bot_h - popup_bottom_pad_y - popup_scrollbar_h;
                                    if (thumb_h < 3) thumb_h = 3;
                                    auto thumb_w = std::max(5, thumb_h * popup_ws_thumb_ratio_w / popup_ws_thumb_ratio_h) | 1; // Ensure odd width for centered cross.
                                    auto has_plus = (workspaces_ptr->size() < ws_max_count);
                                    auto thumb_stride = thumb_w + popup_ws_thumb_gap;
                                    auto total_content_w = ws_count * thumb_stride + (has_plus ? thumb_w + popup_ws_thumb_gap : 0);
                                    auto max_scroll = std::max(0, total_content_w - full_w);
                                    if (max_scroll > 0)
                                    {
                                        auto thumb_left = cur * thumb_stride;
                                        auto thumb_right = thumb_left + thumb_w;
                                        auto scroll = *scroll_off_ptr;
                                        if (thumb_left < scroll)
                                            scroll = thumb_left;
                                        else if (thumb_right > scroll + full_w)
                                            scroll = thumb_right - full_w;
                                        *scroll_off_ptr = std::clamp(scroll, 0, max_scroll);
                                    }
                                    ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }
                            // Bottom up/down: ignored (swallow so they don't reach the app under the popup).
                            gear.set_handled(faux);
                            return;
                        }
                        // Label keys: jump to workspace (bottom section) or pane (top section)
                        // whose displayed badge matches the pressed key. Badges are rendered as
                        // char(ws_min_index + i) across the full [ws_min_index .. ws_max_index]
                        // range (0x30..0x7E: '0'-'9', ':;<=>?@', 'A'-'Z', '[\]^_`', 'a'-'z', '{|}~').
                        // The pressed character maps directly to index (ch[0] - ws_min_index).
                        auto& ch = gear.keybd::cluster;
                        if (ch.size() == 1 && ch[0] >= ws_min_index && ch[0] <= ws_max_index)
                        {
                            auto target = (size_t)(ch[0] - ws_min_index);
                            if (*focus_section_ptr == 0) // Top section: focus the matching pane in the previewed ws.
                            {
                                auto panes = gather_top_panes();
                                if (target < panes.size())
                                {
                                    auto slot_veer = panes[target].slot_veer;
                                    auto idx = *preview_idx_ptr;
                                    dismiss_visual();
                                    dismiss_hook();
                                    if (idx < workspaces_ptr->size())
                                    {
                                        switch_workspace(idx);
                                        if (auto focus_target = get_slot_focus_target(slot_veer))
                                        {
                                            pro::focus::set(focus_target, gear.id, solo::on);
                                        }
                                        (*refresh_status_bar_fn)();
                                    }
                                }
                                gear.set_handled(faux);
                                return;
                            }
                            // Bottom section: switch to the matching workspace.
                            if (target < workspaces_ptr->size())
                            {
                                dismiss_visual();
                                dismiss_hook();
                                switch_workspace(target);
                                (*refresh_status_bar_fn)();
                            }
                            gear.set_handled(faux);
                            return;
                        }
                        // Swallow all other keys.
                        gear.set_handled(faux);
                    };
                };

            // Handle a workspace's root-veer last-empty-slot close request:
            // the quit::any release handler on the root node_veer fires a
            // tier::request swap when its last empty slot is closed (count == 1).
            // Destroy the workspace; if it was the last one, shutdown the tile.
            workspace_host_ptr->LISTEN(tier::request, e2::form::proceed::swap, item_ptr, -, (workspaces_ptr, destroy_workspace, workspace_host_ptr))
            {
                if (!item_ptr) return;
                // Find and destroy the workspace that matches item_ptr.
                // Capture the sptr (not the index) to avoid stale-index bugs
                // if multiple destroys are enqueued in the same event cycle.
                auto victim = item_ptr;
                workspace_host_ptr->base::enqueue([victim, destroy_workspace, workspaces_ptr](auto& /*boss*/) mutable
                {
                    for (auto i = size_t{}; i < workspaces_ptr->size(); i++)
                    {
                        if ((*workspaces_ptr)[i] == victim)
                        {
                            destroy_workspace(i);
                            return;
                        }
                    }
                });
                // Leave item_ptr unchanged so the originating root_veer does not riseup a further swap.
            };

            // Initial status bar paint.
            (*refresh_status_bar_fn)();
            object->invoke([&](auto& boss)
                {
                    auto& root_veer = boss.base::field(std::function<ui::veer&()>(
                        [wp = workspaces_ptr, ip = current_ws_index_ptr, dead = netxs::sptr<ui::veer>{}]() mutable -> ui::veer&
                        {
                            if (wp->empty()) // Guard: between last destroy and shutdown teardown.
                            {
                                if (!dead) dead = ui::veer::ctor();
                                return *dead;
                            }
                            auto idx = std::min(*ip, wp->size() - 1);
                            return *(*wp)[idx];
                        }));
                    auto& foreach = boss.base::field([&](id_t gear_id, auto proc)
                    {
                        auto root_veer_ptr = root_veer().base::This();
                        _foreach(_foreach, root_veer_ptr, gear_id, proc);
                    });
                    auto& nothing_to_iterate = boss.base::field([&]
                    {
                        return root_veer().back()->root();
                    });
                    auto& oneshot = boss.base::field(hook{});
                    boss.LISTEN(tier::anycast, e2::form::upon::created, gear, oneshot)
                    {
                        auto& gate = gear.owner;
                        auto& current_default = gate.base::property("desktop.selected");
                        auto world_ptr = boss.base::signal(tier::general, e2::config::creator);
                        auto conf_list_ptr = world_ptr->base::signal(tier::request, desk::events::menu);
                        auto& conf_list = *conf_list_ptr;
                        auto& config = conf_list[current_default];
                        if (config.type == app::tile::id) // Reset the currently selected application to the previous one.
                        {
                            auto& previous_default = gate.base::property("desktop.prev_selected");
                            current_default = previous_default;
                            gate.base::signal(tier::release, e2::data::changed, previous_default); // Signal to update UI.
                        }
                        boss.base::unfield(oneshot);
                    };
                    boss.LISTEN(tier::anycast, e2::form::upon::started, root_ptr)
                    {
                        if (root_ptr)
                        {
                            if (auto world_ptr = boss.base::signal(tier::general, e2::config::creator))
                            {
                                boss.base::signal(tier::anycast, vtm::events::attached, world_ptr);
                            }
                            else
                            {
                                // Standalone mode: Set focus to the first focusable child element
                                // This ensures keyboard input works correctly in standalone tile mode
                                if (root_veer().count() > 0)
                                {
                                    pro::focus::set(root_veer().back(), id_t{}, solo::on);
                                }
                            }
                        }
                    };
                    boss.LISTEN(tier::request, e2::form::prop::window::state, state)
                    {
                        state = winstate::tiled;
                    };
                    boss.LISTEN(tier::request, e2::form::prop::window::statesrc, window_ptr)
                    {
                        window_ptr = boss.This();
                    };
                    boss.LISTEN(tier::preview, e2::form::prop::cwd, path)
                    {
                        boss.base::signal(tier::anycast, e2::form::prop::cwd, path);
                    };
                    // Note: the `e2::form::proceed::swap` request from a workspace's
                    // root node_veer (when its last empty slot is closed) is intercepted
                    // at `workspace_host` and converted into a destroy_workspace() call;
                    // shutdown happens when the last workspace is destroyed.
                    auto& luafx = boss.bell::indexer.luafx;
                    tile_context = config.settings::push_context("/config/events/tile/");
                    auto script_list = config.settings::take_ptr_list_for_name("script");
                    auto bindings = input::bindings::load(config, script_list);
                    input::bindings::keybind(boss, bindings);
                    boss.base::add_methods(basename::tile,
                    {
                        { methods::FocusNextPaneOrGrip, [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                auto dir = luafx.get_args_or(1, si32{ 1 });
                                                                dir < 0 ? boss.base::signal(tier::preview, app::tile::events::ui::focus::prev, gear)
                                                                        : boss.base::signal(tier::preview, app::tile::events::ui::focus::next, gear);
                                                            });
                                                        }},
                        { methods::FocusNextPane,       [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                auto dir = luafx.get_args_or(1, si32{ 1 });
                                                                dir < 0 ? boss.base::signal(tier::preview, app::tile::events::ui::focus::prevpane, gear)
                                                                        : boss.base::signal(tier::preview, app::tile::events::ui::focus::nextpane, gear);
                                                            });
                                                        }},
                        { methods::LastPane,            [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::focus::lastpane, gear);
                                                            });
                                                        }},
                        { methods::FocusNextGrip,       [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                auto dir = luafx.get_args_or(1, si32{ 1 });
                                                                dir < 0 ? boss.base::signal(tier::preview, app::tile::events::ui::focus::prevgrip, gear)
                                                                        : boss.base::signal(tier::preview, app::tile::events::ui::focus::nextgrip, gear);
                                                            });
                                                        }},
                        { methods::FocusLeftPane,       [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::focus::leftpane, gear);
                                                            });
                                                        }},
                        { methods::FocusRightPane,      [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::focus::rightpane, gear);
                                                            });
                                                        }},
                        { methods::FocusUpPane,         [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::focus::uppane, gear);
                                                            });
                                                        }},
                        { methods::FocusDownPane,       [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::focus::downpane, gear);
                                                            });
                                                        }},
                        { methods::RunApplication,      [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                //todo add agrs
                                                                //auto dir = luafx.get_args_or(1, si32{ 1 });
                                                                boss.base::signal(tier::preview, app::tile::events::ui::create, gear);
                                                            });
                                                        }},
                        { methods::ReRunApplication,    [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::rerun, gear);
                                                            });
                                                        }},
                        { methods::SelectApplication,   [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                auto dir = luafx.get_args_or(1, si32{ 1 });
                                                                boss.base::signal(tier::preview, app::tile::events::ui::selectapp, { gear, dir });
                                                            });
                                                        }},
                        { methods::SelectedApp,         [&]
                                                        {
                                                            auto state = app::tile::events::app_state{};
                                                            boss.base::signal(tier::preview, app::tile::events::ui::selected_app, &state);
                                                            luafx.set_return(state.label);
                                                        }},
                        { methods::SetSelectedApp,      [&]
                                                        {
                                                            auto new_id = luafx.get_args_or(1, text{});
                                                            if (!new_id.empty())
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::setapp, new_id);
                                                            }
                                                        }},
                        { methods::SelectAllPanes,      [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::select, gear);
                                                            });
                                                        }},
                        { methods::SplitPane,           [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                auto dir = luafx.get_args_or(1, si32{ 1 });
                                                                dir > 0 ? boss.base::signal(tier::preview, app::tile::events::ui::split::vt, gear)
                                                                        : boss.base::signal(tier::preview, app::tile::events::ui::split::hz, gear);
                                                            });
                                                        }},
                        { methods::RotateSplit,         [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::rotate, gear);
                                                            });
                                                        }},
                        { methods::SwapPanes,           [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::swap, gear);
                                                            });
                                                        }},
                        { methods::EqualizeSplitRatio,  [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::equalize, gear);
                                                            });
                                                        }},
                        { methods::SetTitle,            [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::title, gear);
                                                            });
                                                        }},
                        { methods::ZoomPane,            [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::zoom, gear);
                                                            });
                                                        }},
                        { methods::ClosePane,           [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::close, gear);
                                                            });
                                                        }},
                        { methods::CloseSlot,           [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::closeslot, gear);
                                                            });
                                                        }},
                        { methods::SetMenuColor,        [&, menu_data]
                                                        {
                                                            auto p_clr = luafx.get_args_or(1, ui32{ 0 });
                                                            auto f_clr = luafx.get_args_or(2, ui32{ 0 });
                                                            if (p_clr && f_clr)
                                                            {
                                                                color_passive.bgc(p_clr);
                                                                color_focused.bgc(f_clr);
                                                                window_clr = is_focused ? color_focused : color_passive;
                                                                menu_data->active()->shader(window_clr);
                                                                boss.base::deface();
                                                            }
                                                        }},
                        { methods::ShowPaneIndex,        [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::focus::paneindex, gear);
                                                            });
                                                        }},
                        { methods::OpenCommandBar,       [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::focus::commandbar, gear);
                                                            });
                                                        }},
                        { methods::PickApplication,      [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                boss.base::signal(tier::preview, app::tile::events::ui::focus::pickapp, gear);
                                                            });
                                                        }},
                        { methods::Disconnect,          [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                gear.owner.base::signal(tier::preview, e2::conio::quit);
                                                            });
                                                        }},
                        { methods::Shutdown,            [&]
                                                        {
                                                            luafx.run_with_gear([&](auto& gear)
                                                            {
                                                                gear.owner.base::signal(tier::general, e2::shutdown, utf::concat(prompt::tile, "Shutdown on signal"));
                                                            });
                                                        }},
                        { methods::CreateWorkspace,     [&, create_workspace]
                                                        {
                                                            auto selected_override = text{ boss.base::property("tile.selected") };
                                                            auto new_idx = create_workspace(selected_override);
                                                            luafx.set_return((si32)new_idx);
                                                        }},
                        { methods::DestroyWorkspace,    [&, destroy_workspace, current_ws_index_ptr]
                                                        {
                                                            auto idx = luafx.get_args_or(1, si32{ -1 });
                                                            if (idx < 0) idx = (si32)*current_ws_index_ptr;
                                                            destroy_workspace((size_t)idx);
                                                        }},
                        { methods::SwitchWorkspace,     [&, switch_workspace]
                                                        {
                                                            auto idx = luafx.get_args_or(1, si32{ 0 });
                                                            switch_workspace((size_t)idx);
                                                        }},
                        { methods::NextWorkspace,       [&, next_workspace]
                                                        {
                                                            next_workspace();
                                                        }},
                        { methods::PrevWorkspace,       [&, prev_workspace]
                                                        {
                                                            prev_workspace();
                                                        }},
                        { methods::LastWorkspace,       [&, last_workspace]
                                                        {
                                                            last_workspace();
                                                        }},
                        { methods::CurrentWorkspace,    [&, current_ws_index_ptr]
                                                        {
                                                            luafx.set_return((si32)*current_ws_index_ptr);
                                                        }},
                        { methods::OpenWorkspacePopup,  [&, open_workspace_popup_fn]
                                                        {
                                                            (*open_workspace_popup_fn)();
                                                        }},
                    });

                    // Install the focused-pane resolver consumed by the
                    // vtm.terminal Lua proxy. Reading is gear-id-driven so
                    // every script invocation (keybind, command bar, prerun)
                    // dispatches to the pane that the calling user currently
                    // owns focus on. Falls back to last-focused, then to a
                    // single tracked slot, mirroring the command bar's own
                    // resolution heuristic for keyboard-shortcut entry where
                    // gear focus history may not yet exist.
                    auto& terminal_proxy_resolver = boss.base::template property<terminal_proxy_resolver_t>(terminal_proxy_resolver_field);
                    terminal_proxy_resolver = [current_focus_history](id_t gear_id) -> ui::sptr
                    {
                        auto focus_history_ptr = current_focus_history();
                        if (!focus_history_ptr) return {};
                        auto slot_ptr = ui::sptr{};
                        if (auto iter = focus_history_ptr->current.find(gear_id);
                            iter != focus_history_ptr->current.end())
                        {
                            slot_ptr = iter->second.lock();
                        }
                        if (!slot_ptr) slot_ptr = focus_history_ptr->last(gear_id);
                        if (!slot_ptr && focus_history_ptr->current.size() == 1)
                        {
                            slot_ptr = focus_history_ptr->current.begin()->second.lock();
                        }
                        if (!slot_ptr) return {};
                        return get_slot_focus_target(slot_ptr);
                    };

                    // Install the broadcaster consumed by the vtm.terminal Lua
                    // proxy. Iterates every applet currently focused for the
                    // gear (i.e. every pane that SelectAllPanes / multi-focus
                    // has marked as "selected") and invokes the visitor on
                    // each, so terminal-side scripts (ToggleFindBar, Paste,
                    // ClearScrollback, ScrollViewport*, ...) fan out to all
                    // selected panes — mirroring the foreach(gear.id, ...)
                    // pattern used by tile-side commands (split, rotate,
                    // close, ...). When no pane is currently focused for the
                    // gear, the proxy falls back to the single-target
                    // resolver above so single-focus and edge-case (very
                    // first keybind, standalone tile) semantics are
                    // preserved.
                    auto& terminal_proxy_broadcaster = boss.base::template property<terminal_proxy_broadcaster_t>(terminal_proxy_broadcaster_field);
                    terminal_proxy_broadcaster = [&foreach](id_t gear_id, terminal_proxy_broadcaster_visitor_t const& visit)
                    {
                        if (gear_id == id_t{}) return; // No active gear: nothing to broadcast to.
                        foreach(gear_id, [&](auto& item_ptr, si32 item_type, auto /*node_veer_ptr*/)
                        {
                            if (item_type != item_type::applet) return;
                            visit(item_ptr);
                        });
                    };

                    // Install the vtm.terminal Lua proxy on the tile
                    // manager's lua_State. The tile manager has no terminal
                    // applet of its own (those live in dtvt child processes
                    // with their own Lua engines), so vtm.terminal.X(...)
                    // would otherwise resolve to nothing and warn. We
                    // override with a metatable that, on any field access,
                    // returns a closure forwarding the call to the focused
                    // dtvt pane via e2::command::run. The dtvt parent-side
                    // listener serializes the script string over the pipe
                    // and the child's gate listener runs it in the
                    // terminal's own Lua engine where vtm.terminal.* is
                    // bound. This makes scripts like
                    //     vtm.terminal.PasteClipboard()
                    // work uniformly from keybinds, command bar, prerun
                    // hooks, etc., without any caller-side routing.
                    {
                        auto* L = luafx.lua;
                        static auto term_proxy_metaindex = std::to_array<luaL_Reg>(
                            {{ "__index",    &tile_terminal_proxy_index },
                             { "__tostring", netxs::events::luna::vtmlua_object2string },
                             { nullptr,      nullptr }});
                        if (::luaL_newmetatable(L, "tile_terminal_proxy_metaindex"))
                        {
                            ::luaL_setfuncs(L, term_proxy_metaindex.data(), 0);
                        }
                        ::lua_pop(L, 1); // Pop the metatable returned by luaL_newmetatable.
                        // Install the proxy as vtm.terminal: rawset on the
                        // global vtm table so Lua finds it before invoking
                        // vtm's __index (which would otherwise fall through
                        // to auth::get_target("terminal", ...) and log a
                        // "no terminal object found" warning).
                        ::lua_getglobal(L, basename::vtm.data());                              // [vtm]
                        ::lua_createtable(L, 0, 0);                                            // [vtm, proxy]
                        ::luaL_setmetatable(L, "tile_terminal_proxy_metaindex");               // [vtm, proxy]
                        ::lua_setfield(L, -2, basename::terminal.data());                      // [vtm]
                        ::lua_pop(L, 1);                                                       // []
                    }

                    boss.LISTEN(tier::preview, app::tile::events::ui::any, gear)
                    {
                        if (boss.bell::protos() == app::tile::events::ui::create.id) return;
                        if (boss.bell::protos() == app::tile::events::ui::zoom.id) return;
                        if (boss.bell::protos() == app::tile::events::ui::selectapp.id) return;
                        if (boss.bell::protos() == app::tile::events::ui::selected_app.id) return;
                        if (root_veer().count() > 2)
                        {
                            root_veer().base::riseup(tier::release, e2::form::proceed::attach); // Restore the window before any action if maximized.
                        }
                    };
                    auto& switch_counter = boss.base::field(std::unordered_map<id_t, feed>{});
                    boss.LISTEN(tier::release, input::events::focus::set::any, seed) // Reset the focus switch counter when it is focused from outside.
                    {
                        switch_counter[seed.gear_id] = {};
                    };
                    auto get_global_rect = [](sptr item_ptr)
                    {
                        auto r = item_ptr->base::area();
                        auto p = item_ptr->base::parent();
                        while (p)
                        {
                            r.coor += p->base::area().coor;
                            p = p->base::parent();
                        }
                        return r;
                    };
                    // Directional 2D pane navigation. See `score_pane_direction` (free function
                    // above) for the scoring rationale and the specific visual-logic regressions
                    // it addresses.
                    auto navigate = [get_global_rect, foreach, nothing_to_iterate](auto& gear, twod dir) mutable
                    {
                        if (nothing_to_iterate()) return;

                        auto src_pane = sptr{};
                        auto src_rect = rect{};

                        // Find focused pane
                        foreach(gear.id, [&](auto& item_ptr, si32 item_type, auto)
                        {
                            if (item_type != item_type::grip && pro::focus::is_focused(item_ptr, gear.id))
                            {
                                src_pane = item_ptr;
                                src_rect = get_global_rect(item_ptr);
                            }
                        });

                        if (!src_pane) return;

                        auto best_pane = sptr{};
                        auto best_score = std::tuple<si64, si64, si64>{ si64max, si64max, si64max };

                        foreach(id_t{}, [&](auto& item_ptr, si32 item_type, auto)
                        {
                            if (item_type == item_type::grip || item_ptr == src_pane) return;
                            auto score = score_pane_direction(src_rect, get_global_rect(item_ptr), dir);
                            if (score && *score < best_score)
                            {
                                best_score = *score;
                                best_pane = item_ptr;
                            }
                        });

                        if (best_pane)
                        {
                            pro::focus::set(best_pane, gear.id, solo::on);
                            gear.set_handled();
                        }
                    };

                    //todo generalize refocusing
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::prev, gear)
                    {
                        if (nothing_to_iterate()) return;
                        auto prev_item_ptr = sptr{};
                        auto next_item_ptr = sptr{};
                        foreach(id_t{}, [&](auto& item_ptr, si32 /*item_type*/, auto)
                        {
                            if (pro::focus::is_focused(item_ptr, gear.id))
                            {
                                prev_item_ptr = next_item_ptr;
                            }
                            next_item_ptr = item_ptr;
                        });
                        auto skip = !prev_item_ptr && gear.shared_event && switch_counter[gear.id] == feed::rev;
                        if (skip) // Give another process a chance to handle this event.
                        {
                            switch_counter[gear.id] = {};
                        }
                        else
                        {
                            if (!prev_item_ptr) // Focused item is at the boundary.
                            {
                                prev_item_ptr = next_item_ptr;
                            }
                            if (prev_item_ptr)
                            {
                                auto& prev_item = *prev_item_ptr;
                                prev_item.base::enqueue([&, gear_id = gear.id](auto& /*boss*/) // Keep the focus tree intact while processing events.
                                {
                                    pro::focus::set(prev_item.This(), gear_id, solo::on);
                                });
                                gear.set_handled();
                                switch_counter[gear.id] = feed::rev;
                            }
                        }
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::next, gear)
                    {
                        if (nothing_to_iterate()) return;
                        auto prev_item_ptr = sptr{};
                        auto next_item_ptr = sptr{};
                        auto temp_item_ptr = sptr{};
                        foreach(id_t{}, [&](auto& item_ptr, si32 /*item_type*/, auto)
                        {
                            if (!temp_item_ptr)
                            {
                                temp_item_ptr = item_ptr; // Fallback item.
                            }
                            if (prev_item_ptr)
                            {
                                std::swap(next_item_ptr, item_ptr); // Interrupt foreach (empty item_ptr).
                            }
                            else if (pro::focus::is_focused(item_ptr, gear.id))
                            {
                                prev_item_ptr = item_ptr;
                            }
                        });
                        auto skip = !next_item_ptr && gear.shared_event && switch_counter[gear.id] == feed::fwd;
                        if (skip) // Give another process a chance to handle this event.
                        {
                            switch_counter[gear.id] = {};
                        }
                        else
                        {
                            if (!next_item_ptr) // Focused item is at the boundary.
                            {
                                next_item_ptr = temp_item_ptr;
                            }
                            if (next_item_ptr)
                            {
                                auto& next_item = *next_item_ptr;
                                next_item.base::enqueue([&, gear_id = gear.id](auto& /*boss*/) // Keep the focus tree intact while processing events.
                                {
                                    pro::focus::set(next_item.This(), gear_id, solo::on);
                                });
                                gear.set_handled();
                                switch_counter[gear.id] = feed::fwd;
                            }
                        }
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::prevpane, gear)
                    {
                        if (nothing_to_iterate()) return;
                        auto prev_item_ptr = sptr{};
                        auto next_item_ptr = sptr{};
                        foreach(id_t{}, [&](auto& item_ptr, si32 item_type, auto)
                        {
                            if (item_type != item_type::grip)
                            {
                                if (pro::focus::is_focused(item_ptr, gear.id))
                                {
                                    prev_item_ptr = next_item_ptr;
                                }
                                next_item_ptr = item_ptr;
                            }
                        });
                        auto skip = !prev_item_ptr && gear.shared_event && switch_counter[gear.id] == feed::rev;
                        if (skip) // Give another process a chance to handle this event.
                        {
                            switch_counter[gear.id] = {};
                        }
                        else
                        {
                            if (!prev_item_ptr) // Focused item is at the boundary.
                            {
                                prev_item_ptr = next_item_ptr;
                            }
                            if (prev_item_ptr)
                            {
                                auto& prev_item = *prev_item_ptr;
                                prev_item.base::enqueue([&, gear_id = gear.id](auto& /*boss*/) // Keep the focus tree intact while processing events.
                                {
                                    pro::focus::set(prev_item.This(), gear_id, solo::on);
                                });
                                gear.set_handled();
                                switch_counter[gear.id] = feed::rev;
                            }
                        }
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::nextpane, gear)
                    {
                        if (nothing_to_iterate()) return;
                        auto prev_item_ptr = sptr{};
                        auto next_item_ptr = sptr{};
                        auto temp_item_ptr = sptr{};
                        foreach(id_t{}, [&](auto& item_ptr, si32 item_type, auto)
                        {
                            if (item_type != item_type::grip)
                            {
                                if (!temp_item_ptr)
                                {
                                    temp_item_ptr = item_ptr; // Fallback item.
                                }
                                if (prev_item_ptr)
                                {
                                    std::swap(next_item_ptr, item_ptr); // Interrupt foreach (empty item_ptr).
                                }
                                else if (pro::focus::is_focused(item_ptr, gear.id))
                                {
                                    prev_item_ptr = item_ptr;
                                }
                            }
                        });
                        auto skip = !next_item_ptr && gear.shared_event && switch_counter[gear.id] == feed::fwd;
                        if (skip) // Give another process a chance to handle this event.
                        {
                            switch_counter[gear.id] = {};
                        }
                        else
                        {
                            if (!next_item_ptr) // Focused item is at the boundary.
                            {
                                next_item_ptr = temp_item_ptr;
                            }
                            if (next_item_ptr)
                            {
                                auto& next_item = *next_item_ptr;
                                next_item.base::enqueue([&, gear_id = gear.id](auto& /*boss*/) // Keep the focus tree intact while processing events.
                                {
                                    pro::focus::set(next_item.This(), gear_id, solo::on);
                                });
                                gear.set_handled();
                                switch_counter[gear.id] = feed::fwd;
                            }
                        }
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::lastpane, gear, -, (current_focus_history))
                    {
                        auto focus_history_ptr = current_focus_history();
                        if (!focus_history_ptr) return;
                        if (auto slot_ptr = focus_history_ptr->last(gear.id))
                        if (auto item_ptr = get_slot_focus_target(slot_ptr))
                        {
                            item_ptr->base::enqueue([item_ptr, gear_id = gear.id](auto& /*boss*/)
                            {
                                pro::focus::set(item_ptr, gear_id, solo::on);
                            });
                        }
                        gear.set_handled();
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::prevgrip, gear)
                    {
                        if (nothing_to_iterate()) return;
                        auto prev_grip_ptr = sptr{};
                        auto next_grip_ptr = sptr{};
                        foreach(id_t{}, [&](auto& item_ptr, si32 item_type, auto)
                        {
                            if (item_type == item_type::grip)
                            {
                                if (pro::focus::is_focused(item_ptr, gear.id))
                                {
                                    prev_grip_ptr = next_grip_ptr;
                                }
                                next_grip_ptr = item_ptr;
                            }
                        });
                        if (!prev_grip_ptr) // Focused grip is at the boundary.
                        {
                            prev_grip_ptr = next_grip_ptr;
                        }
                        if (prev_grip_ptr)
                        {
                            auto& prev_grip = *prev_grip_ptr;
                            prev_grip.base::enqueue([&, gear_id = gear.id](auto& /*boss*/) // Keep the focus tree intact while processing events.
                            {
                                pro::focus::set(prev_grip.This(), gear_id, solo::on);
                            });
                            gear.set_handled();
                        }
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::nextgrip, gear)
                    {
                        if (nothing_to_iterate()) return;
                        auto prev_grip_ptr = sptr{};
                        auto next_grip_ptr = sptr{};
                        auto temp_grip_ptr = sptr{};
                        foreach(id_t{}, [&](auto& item_ptr, si32 item_type, auto)
                        {
                            if (item_type == item_type::grip)
                            {
                                if (!temp_grip_ptr)
                                {
                                    temp_grip_ptr = item_ptr; // Fallback item.
                                }
                                if (prev_grip_ptr)
                                {
                                    std::swap(next_grip_ptr, item_ptr); // Interrupt foreach (empty item_ptr).
                                }
                                else if (pro::focus::is_focused(item_ptr, gear.id))
                                {
                                    prev_grip_ptr = item_ptr;
                                }
                            }
                        });
                        if (!next_grip_ptr) // Focused item is at the boundary.
                        {
                            next_grip_ptr = temp_grip_ptr;
                        }
                        if (next_grip_ptr)
                        {
                            auto& next_grip = *next_grip_ptr;
                            next_grip.base::enqueue([&, gear_id = gear.id](auto& /*boss*/) // Keep the focus tree intact while processing events.
                            {
                                pro::focus::set(next_grip.This(), gear_id, solo::on);
                            });
                            gear.set_handled();
                        }
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::leftpane, gear, boss.sensors, (navigate))
                    {
                        navigate(gear, twod{ -1, 0 });
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::rightpane, gear, boss.sensors, (navigate))
                    {
                        navigate(gear, twod{ 1, 0 });
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::uppane, gear, boss.sensors, (navigate))
                    {
                        navigate(gear, twod{ 0, -1 });
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::downpane, gear, boss.sensors, (navigate))
                    {
                        navigate(gear, twod{ 0, 1 });
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::paneindex, gear, -, (pane_index_active, wrapper_shadow = ptr::shadow(wrapper)))
                    {
                        if (*pane_index_active) { gear.set_handled(); return; }
                        auto wrapper_ptr = wrapper_shadow.lock();
                        if (!wrapper_ptr) return;
                        if (nothing_to_iterate()) return;

                        // Collect all panes and assign index labels (ASCII 0x30~0x7E: '0'..'~', up to 79 panes).
                        struct pane_info_t { text label; sptr slot; };
                        auto pane_list = ptr::shared(std::vector<pane_info_t>{});
                        auto pane_count = si32{};
                        foreach(id_t{}, [&](auto& /*item_ptr*/, si32 item_type, auto node_veer_ptr)
                        {
                            if (item_type != item_type::grip && pane_count < 0x7E - 0x30 + 1)
                            {
                                auto c = char(0x30 + pane_count);
                                pane_list->push_back({ text(1, c), sptr(node_veer_ptr) });
                                pane_count++;
                            }
                        });
                        if (pane_list->empty()) return;

                        *pane_index_active = true;

                        // Helper: compute an item's rect relative to the wrapper (overlay coordinate space).
                        auto wrapper_raw = wrapper_ptr.get();
                        auto get_rect_in = [wrapper_raw](sptr const& item_ptr) -> rect
                        {
                            auto r = item_ptr->base::area();
                            auto p = item_ptr->base::parent();
                            while (p && p.get() != wrapper_raw)
                            {
                                r.coor += p->base::area().coor;
                                p = p->base::parent();
                            }
                            return r;
                        };

                        // Build overlay.
                        auto overlay_ptr = ui::mock::ctor();
                        overlay_ptr->invoke([&](auto& ovl)
                        {
                            auto ovl_id = ovl.bell::id;
                            ovl.LISTEN(tier::release, e2::render::any, parent_canvas, -, (pane_list, get_rect_in, ovl_id))
                            {
                                // Dim the entire tile area by halving RGB, preserving original hue.
                                // Set link to overlay id to capture mouse events.
                                parent_canvas.fill([ovl_id](cell& c)
                                {
                                    c.bgc().faint();
                                    c.fgc().faint();
                                    c.cur(text_cursor::none); // Suppress any terminal cursor bleeding through.
                                    c.link(ovl_id);
                                });
                                // Draw index label centered on each pane.
                                // 5x10 pixel bitmap font rendered via half-block characters.
                                // Each cell row encodes 2 pixel rows -> 5 cells wide x 5 cells tall.
                                // Catppuccin Mocha palette: base background + blue glyph.
                                static constexpr si32 gfont_w  = 5;  // bitmap columns (pixels)
                                static constexpr si32 gfont_h  = 10; // bitmap rows    (pixels)
                                static constexpr si32 gcell_w  = 5;  // rendered width  (cells)
                                static constexpr si32 gcell_h  = 5;  // rendered height (cells)
                                static constexpr auto label_bg = 0xFF1E1E2E; // Catppuccin Mocha Base
                                static constexpr auto label_fg = 0xFF89B4FA; // Catppuccin Mocha Blue
                                // 5x10 bitmaps: 10 rows per glyph, 5 bits per row (MSB = leftmost pixel).
                                static constexpr si32 gfont[][gfont_h] =
                                {                                                          // idx  char
                                    { 0x0E,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0x00 }, //  0  '0'
                                    { 0x04,0x0C,0x04,0x04,0x04,0x04,0x04,0x04,0x0E,0x00 }, //  1  '1'
                                    { 0x0E,0x11,0x01,0x01,0x02,0x04,0x08,0x10,0x1F,0x00 }, //  2  '2'
                                    { 0x0E,0x11,0x01,0x01,0x0E,0x01,0x01,0x11,0x0E,0x00 }, //  3  '3'
                                    { 0x01,0x03,0x05,0x09,0x11,0x1F,0x01,0x01,0x01,0x00 }, //  4  '4'
                                    { 0x1F,0x10,0x10,0x1E,0x01,0x01,0x01,0x11,0x0E,0x00 }, //  5  '5'
                                    { 0x0E,0x11,0x10,0x10,0x1E,0x11,0x11,0x11,0x0E,0x00 }, //  6  '6'
                                    { 0x1F,0x01,0x01,0x02,0x02,0x04,0x04,0x04,0x04,0x00 }, //  7  '7'
                                    { 0x0E,0x11,0x11,0x11,0x0E,0x11,0x11,0x11,0x0E,0x00 }, //  8  '8'
                                    { 0x0E,0x11,0x11,0x11,0x0F,0x01,0x01,0x11,0x0E,0x00 }, //  9  '9'
                                    { 0x0E,0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0x11,0x00 }, // 10  'A'
                                    { 0x1E,0x11,0x11,0x11,0x1E,0x11,0x11,0x11,0x1E,0x00 }, // 11  'B'
                                    { 0x0E,0x11,0x10,0x10,0x10,0x10,0x10,0x11,0x0E,0x00 }, // 12  'C'
                                    { 0x1E,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x1E,0x00 }, // 13  'D'
                                    { 0x1F,0x10,0x10,0x10,0x1E,0x10,0x10,0x10,0x1F,0x00 }, // 14  'E'
                                    { 0x1F,0x10,0x10,0x10,0x1E,0x10,0x10,0x10,0x10,0x00 }, // 15  'F'
                                    { 0x0E,0x11,0x10,0x10,0x17,0x11,0x11,0x11,0x0E,0x00 }, // 16  'G'
                                    { 0x11,0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0x11,0x00 }, // 17  'H'
                                    { 0x0E,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x0E,0x00 }, // 18  'I'
                                    { 0x07,0x01,0x01,0x01,0x01,0x01,0x01,0x11,0x0E,0x00 }, // 19  'J'
                                    { 0x11,0x11,0x12,0x14,0x18,0x14,0x12,0x11,0x11,0x00 }, // 20  'K'
                                    { 0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x1F,0x00 }, // 21  'L'
                                    { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11,0x11,0x11,0x00 }, // 22  'M'
                                    { 0x11,0x19,0x19,0x15,0x15,0x13,0x13,0x11,0x11,0x00 }, // 23  'N'
                                    { 0x0E,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0x00 }, // 24  'O'
                                    { 0x1E,0x11,0x11,0x11,0x1E,0x10,0x10,0x10,0x10,0x00 }, // 25  'P'
                                    { 0x0E,0x11,0x11,0x11,0x11,0x11,0x15,0x12,0x0D,0x00 }, // 26  'Q'
                                    { 0x1E,0x11,0x11,0x11,0x1E,0x14,0x12,0x11,0x11,0x00 }, // 27  'R'
                                    { 0x0E,0x11,0x10,0x10,0x0E,0x01,0x01,0x11,0x0E,0x00 }, // 28  'S'
                                    { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00 }, // 29  'T'
                                    { 0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0x00 }, // 30  'U'
                                    { 0x11,0x11,0x11,0x11,0x11,0x11,0x0A,0x0A,0x04,0x00 }, // 31  'V'
                                    { 0x11,0x11,0x11,0x11,0x11,0x15,0x15,0x1B,0x0A,0x00 }, // 32  'W'
                                    { 0x11,0x11,0x0A,0x0A,0x04,0x0A,0x0A,0x11,0x11,0x00 }, // 33  'X'
                                    { 0x11,0x11,0x0A,0x0A,0x04,0x04,0x04,0x04,0x04,0x00 }, // 34  'Y'
                                    { 0x1F,0x01,0x01,0x02,0x04,0x08,0x10,0x10,0x1F,0x00 }, // 35  'Z'
                                    { 0x00,0x04,0x04,0x00,0x00,0x00,0x04,0x04,0x00,0x00 }, // 36  ':'
                                    { 0x00,0x04,0x04,0x00,0x00,0x00,0x04,0x04,0x08,0x00 }, // 37  ';'
                                    { 0x00,0x02,0x04,0x08,0x10,0x08,0x04,0x02,0x00,0x00 }, // 38  '<'
                                    { 0x00,0x00,0x00,0x1F,0x00,0x00,0x1F,0x00,0x00,0x00 }, // 39  '='
                                    { 0x00,0x08,0x04,0x02,0x01,0x02,0x04,0x08,0x00,0x00 }, // 40  '>'
                                    { 0x0E,0x11,0x01,0x01,0x02,0x04,0x04,0x00,0x04,0x00 }, // 41  '?'
                                    { 0x0E,0x11,0x11,0x17,0x15,0x15,0x13,0x10,0x0F,0x00 }, // 42  '@'
                                    { 0x0E,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x0E,0x00 }, // 43  '['
                                    { 0x10,0x10,0x08,0x08,0x04,0x04,0x02,0x02,0x01,0x00 }, // 44  '\'
                                    { 0x0E,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x0E,0x00 }, // 45  ']'
                                    { 0x04,0x0A,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, // 46  '^'
                                    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x00 }, // 47  '_'
                                    { 0x08,0x04,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, // 48  '`'
                                    { 0x06,0x04,0x04,0x04,0x08,0x04,0x04,0x04,0x06,0x00 }, // 49  '{'
                                    { 0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00 }, // 50  '|'
                                    { 0x0C,0x04,0x04,0x04,0x02,0x04,0x04,0x04,0x0C,0x00 }, // 51  '}'
                                    { 0x00,0x00,0x00,0x09,0x16,0x00,0x00,0x00,0x00,0x00 }, // 52  '~'
                                    { 0x00,0x00,0x00,0x0E,0x01,0x0F,0x11,0x11,0x0F,0x00 }, // 53  'a'
                                    { 0x10,0x10,0x10,0x1E,0x11,0x11,0x11,0x11,0x1E,0x00 }, // 54  'b'
                                    { 0x00,0x00,0x00,0x0E,0x11,0x10,0x10,0x11,0x0E,0x00 }, // 55  'c'
                                    { 0x01,0x01,0x01,0x0F,0x11,0x11,0x11,0x11,0x0F,0x00 }, // 56  'd'
                                    { 0x00,0x00,0x00,0x0E,0x11,0x1F,0x10,0x10,0x0E,0x00 }, // 57  'e'
                                    { 0x06,0x09,0x08,0x1E,0x08,0x08,0x08,0x08,0x08,0x00 }, // 58  'f'
                                    { 0x00,0x00,0x00,0x0F,0x11,0x11,0x11,0x0F,0x01,0x0E }, // 59  'g'
                                    { 0x10,0x10,0x10,0x16,0x19,0x11,0x11,0x11,0x11,0x00 }, // 60  'h'
                                    { 0x00,0x04,0x00,0x0C,0x04,0x04,0x04,0x04,0x0E,0x00 }, // 61  'i'
                                    { 0x00,0x04,0x00,0x0C,0x04,0x04,0x04,0x04,0x04,0x08 }, // 62  'j'
                                    { 0x10,0x10,0x10,0x12,0x14,0x18,0x14,0x12,0x11,0x00 }, // 63  'k'
                                    { 0x0C,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x0E,0x00 }, // 64  'l'
                                    { 0x00,0x00,0x00,0x1B,0x15,0x15,0x15,0x15,0x15,0x00 }, // 65  'm'
                                    { 0x00,0x00,0x00,0x16,0x19,0x11,0x11,0x11,0x11,0x00 }, // 66  'n'
                                    { 0x00,0x00,0x00,0x0E,0x11,0x11,0x11,0x11,0x0E,0x00 }, // 67  'o'
                                    { 0x00,0x00,0x00,0x1E,0x11,0x11,0x11,0x1E,0x10,0x10 }, // 68  'p'
                                    { 0x00,0x00,0x00,0x0F,0x11,0x11,0x11,0x0F,0x01,0x01 }, // 69  'q'
                                    { 0x00,0x00,0x00,0x16,0x19,0x10,0x10,0x10,0x10,0x00 }, // 70  'r'
                                    { 0x00,0x00,0x00,0x0E,0x10,0x0E,0x01,0x01,0x0E,0x00 }, // 71  's'
                                    { 0x00,0x04,0x04,0x0E,0x04,0x04,0x04,0x04,0x03,0x00 }, // 72  't'
                                    { 0x00,0x00,0x00,0x11,0x11,0x11,0x11,0x11,0x0F,0x00 }, // 73  'u'
                                    { 0x00,0x00,0x00,0x11,0x11,0x11,0x0A,0x0A,0x04,0x00 }, // 74  'v'
                                    { 0x00,0x00,0x00,0x11,0x11,0x15,0x15,0x15,0x0A,0x00 }, // 75  'w'
                                    { 0x00,0x00,0x00,0x11,0x0A,0x04,0x04,0x0A,0x11,0x00 }, // 76  'x'
                                    { 0x00,0x00,0x00,0x11,0x11,0x0A,0x0A,0x04,0x08,0x10 }, // 77  'y'
                                    { 0x00,0x00,0x00,0x1F,0x01,0x02,0x04,0x08,0x1F,0x00 }, // 78  'z'
                                };
                                auto glyph_idx = [](char ch) -> si32
                                {
                                    if (ch >= '0' && ch <= '9') return ch - '0';
                                    if (ch >= 'A' && ch <= 'Z') return 10 + ch - 'A';
                                    if (ch >= 'a' && ch <= 'z') return 53 + ch - 'a';
                                    if (ch >= ':' && ch <= '@') return 36 + ch - ':';
                                    if (ch >= '[' && ch <= '`') return 43 + ch - '[';
                                    if (ch >= '{' && ch <= '~') return 49 + ch - '{';
                                    return -1;
                                };
                                for (auto& pane : *pane_list)
                                {
                                    auto r = get_rect_in(pane.slot);
                                    if (r.size.x < 1 || r.size.y < 1) continue;
                                    auto ch = pane.label[0];
                                    auto gi = glyph_idx(ch);
                                    // Render half-block glyph when pane fits 5x5 cells + 1-cell padding.
                                    if (gi >= 0 && r.size.x >= gcell_w + 2 && r.size.y >= gcell_h + 2)
                                    {
                                        auto gx = r.coor.x + (r.size.x - gcell_w) / 2;
                                        auto gy = r.coor.y + (r.size.y - gcell_h) / 2;
                                        // Background pad behind glyph.
                                        parent_canvas.fill(rect{{ gx - 1, gy - 1 }, { gcell_w + 2, gcell_h + 2 }}, [ovl_id](cell& c)
                                        {
                                            c.bgc(label_bg).fgc(label_fg).txt(whitespace).link(ovl_id);
                                        });
                                        // Render glyph: each cell row = 2 pixel rows via half-block chars.
                                        for (auto cy = si32{}; cy < gcell_h; cy++)
                                        {
                                            auto row_top = gfont[gi][cy * 2];
                                            auto row_bot = gfont[gi][cy * 2 + 1];
                                            for (auto cx = si32{}; cx < gcell_w; cx++)
                                            {
                                                auto top = (row_top >> (gfont_w - 1 - cx)) & 1;
                                                auto bot = (row_bot >> (gfont_w - 1 - cx)) & 1;
                                                if (!top && !bot) continue;
                                                parent_canvas.fill(rect{{ gx + cx, gy + cy }, { 1, 1 }}, [=](cell& c)
                                                {
                                                    if      (top && bot) c.bgc(label_fg).txt(whitespace);
                                                    else if (top)        c.bgc(label_bg).fgc(label_fg).txt("\xe2\x96\x80"); // ▀
                                                    else                 c.bgc(label_bg).fgc(label_fg).txt("\xe2\x96\x84"); // ▄
                                                    c.link(ovl_id);
                                                });
                                            }
                                        }
                                    }
                                    else // Fallback: single-character label badge.
                                    {
                                        auto cx = r.coor.x + r.size.x / 2;
                                        auto cy = r.coor.y + r.size.y / 2;
                                        auto bw = std::min(r.size.x, si32{ 5 });
                                        auto bh = std::min(r.size.y, si32{ 3 });
                                        auto lx = cx - bw / 2;
                                        auto ly = cy - bh / 2;
                                        parent_canvas.fill(rect{{ lx, ly }, { bw, bh }}, [ovl_id](cell& c)
                                        {
                                            c.bgc(label_bg).fgc(label_fg).txt(whitespace).link(ovl_id);
                                        });
                                        parent_canvas.fill(rect{{ cx, cy }, { 1, 1 }}, [&pane, ovl_id](cell& c)
                                        {
                                            c.txt(pane.label).link(ovl_id);
                                        });
                                    }
                                }
                            };
                        });
                        wrapper_ptr->attach(overlay_ptr);
                        wrapper_ptr->base::reflow();
                        wrapper_ptr->base::deface();

                        // Keyboard interceptor: jump to pane on index key, dismiss on Esc or any other key.
                        auto kbd_hook = ptr::shared<hook>();
                        auto overlay_shadow = ptr::shadow(overlay_ptr);
                        auto pending_unhook = ptr::shared(faux);
                        auto dismiss_visual = [overlay_shadow, pane_index_active, pending_unhook]
                        {
                            *pending_unhook = true;
                            *pane_index_active = faux;
                            if (auto p = overlay_shadow.lock()) p->base::detach();
                        };
                        auto dismiss_hook = [kbd_hook]{ kbd_hook->reset(); };

                        wrapper_ptr->bell::submit(tier::preview, input::events::keybd::any, *kbd_hook)
                            = [pane_list, dismiss_visual, dismiss_hook, pending_unhook](hids& gear) mutable
                        {
                            if (gear.payload != input::keybd::type::keypress) return;
                            if (gear.keystat == input::key::interrupted) return;
                            if (gear.keybd::handled) return;

                            // After visual dismiss, swallow trailing events until key release.
                            if (*pending_unhook)
                            {
                                if (gear.keystat == input::key::released)
                                {
                                    dismiss_hook();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            // Swallow key-release events (e.g. from the trigger key).
                            if (gear.keystat == input::key::released)
                            {
                                gear.set_handled(faux);
                                return;
                            }

                            // Esc — dismiss without jumping.
                            if (gear.keybd::generic() == input::key::Esc)
                            {
                                dismiss_visual();
                                gear.set_handled(faux);
                                return;
                            }

                            // Ignore modifier-only presses (empty cluster).
                            auto& ch = gear.keybd::cluster;
                            if (ch.empty())
                            {
                                gear.set_handled(faux);
                                return;
                            }

                            // Match index key (ASCII 0x30~0x7E).
                            if (ch.size() == 1)
                            {
                                auto c = ch[0];
                                for (auto& pane : *pane_list)
                                {
                                    if (pane.label[0] == c)
                                    {
                                        if (auto focus_target = get_slot_focus_target(pane.slot))
                                        {
                                            pro::focus::set(focus_target, gear.id, solo::on);
                                        }
                                        break;
                                    }
                                }
                            }

                            // Dismiss overlay after any recognized key.
                            dismiss_visual();
                            gear.set_handled(faux);
                        };

                        gear.set_handled();
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::commandbar, gear, -, (command_bar_active, pending_cmd_list_ptr, pending_cmd_flags_ptr, wrapper_shadow = ptr::shadow(wrapper)))
                    {
                        if (*command_bar_active) { gear.set_handled(); return; }
                        auto wrapper_ptr = wrapper_shadow.lock();
                        if (!wrapper_ptr) return;

                        // If a picker has staged a custom item list (e.g. from focus::pickapp),
                        // use it; otherwise load the default command list from config.
                        auto cmd_list = *pending_cmd_list_ptr ? *pending_cmd_list_ptr
                                                              : command_bar::load(boss.bell::indexer.config);
                        *pending_cmd_list_ptr = {};
                        auto cmd_flags = *pending_cmd_flags_ptr;
                        *pending_cmd_flags_ptr = command_bar::flags::none;
                        if (cmd_list->empty()) return;

                        *command_bar_active = true;

                        // Shared state.
                        auto query_ptr             = ptr::shared(text{});
                        auto caret_cp_ptr          = ptr::shared(si32{ 0 });
                        auto sel_idx_ptr           = ptr::shared(si32{ 0 });
                        auto v_scroll_off_ptr      = ptr::shared(si32{ 0 });
                        auto h_scroll_off_ptr      = ptr::shared(si32{ 0 });
                        auto list_h_scroll_off_ptr = ptr::shared(si32{ 0 }); // List horizontal scroll.
                        auto hover_row_ptr     = ptr::shared(si32{ -1 });
                        auto hover_vsb_ptr     = ptr::shared(faux);
                        auto hover_hsb_ptr     = ptr::shared(faux);
                        auto dragging_vsb_ptr  = ptr::shared(faux);
                        auto drag_vsb_grab_ptr = ptr::shared(si32{ 0 });
                        auto dragging_hsb_ptr  = ptr::shared(faux);
                        auto drag_hsb_grab_ptr = ptr::shared(si32{ 0 });
                        // Keyboard/mouse priority lock: coord captured at the moment of the last
                        // keyboard action. While MouseMove reports the same coord, hover-derived
                        // updates (including sel_idx) are suppressed so a stationary cursor cannot
                        // override keyboard navigation. Initialized to an out-of-range sentinel so
                        // the first real MouseMove is always honored.
                        auto kbd_lock_coord_ptr = ptr::shared(twod{ -32768, -32768 });
                        // Initialization gate: the very first MouseMove after the command bar opens
                        // is silently discarded and its coord is saved into kbd_lock_coord_ptr.
                        // This prevents a cursor that was already resting on an item from
                        // immediately highlighting it on entry (e.g. opened via keyboard shortcut).
                        auto popup_ready_ptr    = ptr::shared(faux);

                        // Pickapp-only "Enter mode" radio buttons in the input row.
                        // Visible only when the session opts into both split and replace shortcuts
                        // (currently the focus::pickapp path).  Each button toggles a mutually
                        // exclusive mode that, while active, rewrites the Enter behaviour:
                        //   enter_mode_none    : default (run script as-is)
                        //   enter_mode_rerun   : append "vtm.tile.ReRunApplication();"  (the "+")
                        //   enter_mode_newws   : append "vtm.tile.CreateWorkspace();"   (the "⬒")
                        //   enter_mode_split_h : append "vtm.tile.SplitPane(0);"        (the "|")
                        //   enter_mode_split_v : append "vtm.tile.SplitPane(1);"        (the "-")
                        // Clicking the currently-active button cancels it (back to enter_mode_none).
                        auto enter_mode_ptr = ptr::shared(si32{ 0 }); // 0=none / 1=rerun / 2=newws / 3=split_h / 4=split_v
                        auto hover_btn_ptr  = ptr::shared(si32{ 0 }); // 0=none / 1/2/3/4 = which mode button is hovered

                        // Build overlay.
                        auto overlay_ptr = ui::mock::ctor();
                        auto overlay_shadow = ptr::shadow(overlay_ptr);
                        auto kbd_hook = ptr::shared<hook>();
                        auto pending_unhook = ptr::shared(faux);
                        auto boss_shadow = ptr::shadow(boss.This());

                        auto dismiss_visual = [overlay_shadow, command_bar_active, pending_unhook]
                        {
                            if (*pending_unhook) return;
                            *pending_unhook     = true;
                            *command_bar_active = faux;
                            if (auto p = overlay_shadow.lock()) p->base::detach();
                        };
                        auto dismiss_hook = [kbd_hook]{ kbd_hook->reset(); };

                        // Dispatch the selected command script. The
                        // vtm.terminal proxy installed on the tile boss's
                        // lua_State (see tile_terminal_proxy_index in
                        // app::tile) intercepts vtm.terminal.* calls and
                        // forwards them to the focused dtvt pane via
                        // e2::command::run, so we no longer need to scan
                        // the script source for routing hints here. Plain
                        // vtm.tile.*, vtm.desktop.* and bare Lua just run
                        // in-process against the tile manager's own engine.
                        auto dispatch_script = [boss_shadow](text const& script, hids& gear)
                        {
                            if (script.empty()) return;
                            auto boss_ptr = boss_shadow.lock();
                            if (!boss_ptr) return;
                            auto& luafx = boss_ptr->bell::indexer.luafx;
                            luafx.set_gear(gear);
                            luafx.run_script(*boss_ptr, script);
                        };

                        overlay_ptr->invoke([&](auto& ovl)
                        {
                            auto ovl_id = ovl.bell::id;

                            // Render callback.
                            ovl.LISTEN(tier::release, e2::render::any, parent_canvas, -,
                                (cmd_list, query_ptr, caret_cp_ptr, sel_idx_ptr,
                                 v_scroll_off_ptr, h_scroll_off_ptr,
                                 hover_vsb_ptr, hover_hsb_ptr,
                                 dragging_vsb_ptr, dragging_hsb_ptr,
                                 list_h_scroll_off_ptr, ovl_id,
                                 enter_mode_ptr, hover_btn_ptr, cmd_flags))
                            {
                                static constexpr auto cb_bg              = command_bar::bg;
                                static constexpr auto cb_surface         = command_bar::surface;
                                static constexpr auto cb_text            = command_bar::text_fg;
                                static constexpr auto cb_subtext         = command_bar::subtext;
                                static constexpr auto cb_prompt          = command_bar::prompt_fg;
                                static constexpr auto cb_vsb_track       = command_bar::scroll_track;
                                static constexpr auto cb_vsb_thumb       = command_bar::scroll_thumb;
                                static constexpr auto cb_vsb_thumb_hover = command_bar::scroll_hover;
                                static constexpr auto cb_vsb_thumb_drag  = command_bar::scroll_drag;
                                static constexpr auto cb_match_fg        = command_bar::match_fg;

                                // Dim entire tile area.
                                parent_canvas.fill([ovl_id](cell& c)
                                {
                                    c.bgc().faint();
                                    c.fgc().faint();
                                    c.cur(text_cursor::none);
                                    c.link(ovl_id);
                                });

                                auto& query        = *query_ptr;
                                auto  sel_idx      = *sel_idx_ptr;
                                auto  hover_vsb    = *hover_vsb_ptr;
                                auto  hover_hsb    = *hover_hsb_ptr;
                                auto  dragging_vsb = *dragging_vsb_ptr;
                                auto  dragging_hsb = *dragging_hsb_ptr;
                                auto  query_len    = command_bar::cp_len(query);
                                auto  caret_cp     = std::clamp(*caret_cp_ptr, si32{ 0 }, query_len);
                                auto  data = command_bar::build_model(*cmd_list, query);
                                auto  l    = command_bar::layout_of(parent_canvas.area().size,
                                                                    data,
                                                                    query_len,
                                                                    *v_scroll_off_ptr,
                                                                    *h_scroll_off_ptr,
                                                                    *list_h_scroll_off_ptr);
                                if (!l.ok) return;
                                *v_scroll_off_ptr      = l.v_scroll;
                                *h_scroll_off_ptr      = l.h_scroll;
                                *list_h_scroll_off_ptr = l.list_h_scroll;

                                auto& filtered          = data.filtered;
                                auto  tip_off           = data.tooltip_col;
                                auto  n_visible         = l.n_visible;
                                auto  dlg_w             = l.dlg_w;
                                auto  dlg_h             = l.dlg_h;
                                auto  dlg_x             = l.dlg_x;
                                auto  dlg_y             = l.dlg_y;
                                auto  inner_x           = l.inner_x;
                                auto  vsb_x             = l.vsb_x;
                                auto  entry_disp_w      = l.entry_disp_w;
                                auto  list_disp_w       = l.list_disp_w;
                                auto  list_content_w    = l.list_content_w;
                                auto  has_hsb           = l.has_hsb;
                                auto  list_h_scroll_off = l.list_h_scroll;
                                auto  max_list_hs       = l.max_list_hscroll;
                                auto  list_rows         = l.list_rows;
                                auto  v_scroll_off      = l.v_scroll;
                                auto  h_scroll_off      = l.h_scroll;

                                // Pickapp-only Enter-mode buttons in the input row.
                                // Visible only when the session opts into both split and
                                // replace shortcuts (focus::pickapp path).  Each button is
                                // 3 cells wide (pad + glyph + pad), four buttons total
                                // occupy 12 cells anchored to the right edge of the entry
                                // row (just before vsb_x).  Hidden whenever the entry
                                // would be left with fewer than 4 input cells (responsive
                                // degradation: all four buttons disappear together).
                                static constexpr auto enter_btns_w     = si32{ 12 };  // 4 buttons × 3 cells each
                                static constexpr auto enter_btn_gap    = si32{ 1 };   // gap between input text and buttons
                                static constexpr auto enter_btns_total = si32{ enter_btns_w + enter_btn_gap }; // = 13
                                static constexpr auto enter_btn_w      = si32{ 3 };
                                static constexpr auto enter_min_input  = si32{ 4 };
                                auto pickapp_caps       = (cmd_flags & command_bar::flags::allow_split)
                                                       && (cmd_flags & command_bar::flags::allow_replace);
                                auto enter_btns_show    = pickapp_caps
                                                       && entry_disp_w >= enter_min_input + enter_btns_total;
                                auto enter_btns_x0      = dlg_x + dlg_w - 1 - enter_btns_w; // leftmost button column
                                auto enter_btn_rerun_cx = enter_btns_x0 + 1;                 // [+] center
                                auto enter_btn_box_cx   = enter_btns_x0 + 4;                 // [⬒] center
                                auto enter_btn_pipe_cx  = enter_btns_x0 + 7;                 // [|] center
                                auto enter_btn_dash_cx  = enter_btns_x0 + 10;                // [-] center
                                auto entry_disp_w_eff  = enter_btns_show ? entry_disp_w - enter_btns_total
                                                                          : entry_disp_w;
                                if (h_scroll_off > caret_cp) h_scroll_off = caret_cp;
                                if (caret_cp - h_scroll_off >= entry_disp_w_eff)
                                {
                                    h_scroll_off = caret_cp - entry_disp_w_eff + 1;
                                }
                                auto max_hscroll_eff = std::max(si32{ 0 }, query_len - entry_disp_w_eff + 1);
                                h_scroll_off = std::clamp(h_scroll_off, si32{ 0 }, max_hscroll_eff);
                                *caret_cp_ptr     = caret_cp;
                                *h_scroll_off_ptr = h_scroll_off;

                                auto put_str = [&](si32 x, si32 y, view str, ui32 fg, ui32 bg, si32 max_cells)
                                {
                                    command_bar::put_text(parent_canvas, ovl_id, x, y, str, fg, bg, max_cells);
                                };
                                auto put_str_skipped = [&](si32 x, si32 y, view str, ui32 fg, ui32 bg, si32 max_cells, si32 skip_cps)
                                {
                                    command_bar::put_text(parent_canvas, ovl_id, x, y, str, fg, bg, max_cells, skip_cps);
                                };
                                auto put_str_highlighted_skipped = [&](si32 x, si32 y, view str, ui32 fg, ui32 match_fg, ui32 bg, si32 max_cells, view query, si32 skip_cps)
                                {
                                    command_bar::put_highlighted(parent_canvas, ovl_id, x, y, str, fg, match_fg, bg, max_cells, query, skip_cps);
                                };

                                // Fill dialog background.
                                parent_canvas.fill(rect{{ dlg_x, dlg_y }, { dlg_w, dlg_h }}, [=, ovl_id = ovl_id](cell& c)
                                {
                                    c.bgc(cb_bg).fgc(cb_text).txt(whitespace).link(ovl_id);
                                });

                                // Upper decoration: ▄ above entry row (fg = entry bg, bg = transparent).
                                if (dlg_y > 0)
                                {
                                    parent_canvas.fill(rect{{ dlg_x, dlg_y - 1 }, { dlg_w, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                    {
                                        c.fgc(cb_surface).txt("\xe2\x96\x84").link(ovl_id); // ▄
                                    });
                                }

                                // Row 0: entry (Surface0 bg, "> " prompt, h-scrolled query text).
                                {
                                    auto entry_y = dlg_y;
                                    parent_canvas.fill(rect{{ dlg_x, entry_y }, { dlg_w, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                    {
                                        c.bgc(cb_surface).fgc(cb_text).txt(whitespace).link(ovl_id);
                                    });
                                    // ">" prompt.
                                    parent_canvas.fill(rect{{ inner_x, entry_y }, { 1, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                    {
                                        c.bgc(cb_surface).fgc(cb_prompt).txt(">").link(ovl_id);
                                    });
                                    // Space after prompt.
                                    parent_canvas.fill(rect{{ inner_x + 1, entry_y }, { 1, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                    {
                                        c.bgc(cb_surface).fgc(cb_text).txt(whitespace).link(ovl_id);
                                    });
                                    // Query text (h-scrolled).
                                    auto q_x = inner_x + 2;
                                    put_str_skipped(q_x, entry_y, query, cb_text, cb_surface, entry_disp_w_eff, h_scroll_off);
                                    // Block cursor at the visible caret position.
                                    auto cursor_vis_x = caret_cp - h_scroll_off;
                                    if (cursor_vis_x >= 0 && cursor_vis_x < entry_disp_w_eff)
                                    {
                                        parent_canvas.fill(rect{{ q_x + cursor_vis_x, entry_y }, { 1, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                        {
                                            c.bgc(cb_text).fgc(cb_surface).link(ovl_id);
                                            if (c.txt().empty() || c.txt() == " ") c.txt(whitespace);
                                        });
                                    }
                                    parent_canvas.fill(rect{{ q_x, entry_y }, { entry_disp_w_eff, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                    {
                                        c.und(unln::line).unc(0).link(ovl_id);
                                    });

                                    // Pickapp-only Enter-mode buttons: [+] [|] [-] anchored
                                    // to the right side of the input row.  Each button is
                                    // a 3-cell hitbox (pad/glyph/pad); the active one is
                                    // drawn with the command-bar selection background to
                                    // mirror find-bar's "active radio" appearance.
                                    if (enter_btns_show)
                                    {
                                        auto enter_mode = *enter_mode_ptr;
                                        auto hover_btn  = *hover_btn_ptr;
                                        auto draw_mode_btn = [&](si32 center_x, view glyph, si32 mode)
                                        {
                                            auto active = enter_mode == mode;
                                            auto hov    = hover_btn == mode;
                                            auto bg     = active ? command_bar::sel_bg
                                                       : hov     ? command_bar::scroll_hover
                                                       :           command_bar::surface;
                                            auto fg     = active ? command_bar::sel_fg
                                                       :           command_bar::text_fg;
                                            auto sp     = text{ " " };
                                            auto g      = text{ glyph };
                                            auto lx     = center_x - 1; // leftmost cell of 3-cell button
                                            for (auto k = si32{ 0 }; k < enter_btn_w; ++k)
                                            {
                                                auto cx = lx + k;
                                                auto& ch = (k == 1) ? g : sp;
                                                parent_canvas.fill(rect{{ cx, entry_y }, { 1, 1 }},
                                                    [=, ovl_id = ovl_id](cell& c)
                                                    {
                                                        c.bgc(bg).fgc(fg).txt(ch).link(ovl_id);
                                                        c.und(unln::none).unc(0);
                                                    });
                                            }
                                        };
                                        draw_mode_btn(enter_btn_rerun_cx, "+",            1); // + ReRunApplication
                                        draw_mode_btn(enter_btn_box_cx,   "\xe2\xac\x92", 2); // ⬒ CreateWorkspace
                                        draw_mode_btn(enter_btn_pipe_cx,  "|",            3); // SplitPane(0) horizontal
                                        draw_mode_btn(enter_btn_dash_cx,  "-",            4); // SplitPane(1) vertical
                                    }
                                }

                                // Row 1: lower separator ▀ (fg = entry bg, bg = list bg).
                                parent_canvas.fill(rect{{ dlg_x, dlg_y + 1 }, { dlg_w, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                {
                                    c.bgc(cb_bg).fgc(cb_surface).txt("\xe2\x96\x80").link(ovl_id); // ▀
                                });

                                // Rows 2..N: command list or "No matches".
                                if (filtered.empty())
                                {
                                    put_str(inner_x, dlg_y + 2, "No matches", cb_subtext, cb_bg, dlg_w - 2);
                                }
                                else
                                {
                                    for (auto vi = si32{}; vi < n_visible; vi++)
                                    {
                                        auto fi       = vi + v_scroll_off;
                                        auto cmd_idx  = filtered[(size_t)fi];
                                        auto row_y    = dlg_y + 2 + vi;
                                        auto is_active = (fi == sel_idx);
                                        auto row_bg    = is_active ? cb_surface : cb_bg;
                                        auto row_fg    = cb_text;
                                        // Fill row background (exclude VSB column).
                                        parent_canvas.fill(rect{{ dlg_x, row_y }, { dlg_w - 1, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                        {
                                            c.bgc(row_bg).fgc(row_fg).txt(whitespace).link(ovl_id);
                                        });
                                        // Indicator prefix: always at inner_x, not affected by horizontal scrolling.
                                        parent_canvas.fill(rect{{ inner_x, row_y }, { 1, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                        {
                                            c.bgc(row_bg).fgc(row_fg).txt(is_active ? "\xe2\x96\xba" : " ").link(ovl_id); // ► or space
                                        });
                                        // Scrollable content (label + tooltip) rendered into columns inner_x+2..vsb_x-1.
                                        // The 2-column prefix (indicator + space) is pinned and never scrolls.
                                        auto lbl_match_fg = cb_match_fg;
                                        auto max_lbl_w_r  = tip_off - 6; // max label display width
                                        auto content_x    = inner_x + 2;
                                        {
                                            // Label: at virtual column 0 of the scrollable area; skip list_h_scroll_off chars.
                                            auto lbl_skip  = list_h_scroll_off;
                                            auto lbl_max_w = std::max(0, std::min(vsb_x - content_x, max_lbl_w_r - lbl_skip));
                                            if (lbl_max_w > 0)
                                                put_str_highlighted_skipped(content_x, row_y, (*cmd_list)[cmd_idx].display, row_fg, lbl_match_fg, row_bg, lbl_max_w, query, lbl_skip);
                                        }
                                        {
                                            // Tooltip: at virtual column (tip_off - 2) of the scrollable area.
                                            auto tip_canvas_x = content_x + (tip_off - 2) - list_h_scroll_off;
                                            auto tip_skip     = std::max(0, content_x - tip_canvas_x);
                                            auto tip_x        = std::max(content_x, tip_canvas_x);
                                            auto tip_max_w    = std::max(0, vsb_x - tip_x);
                                            if (tip_max_w > 0)
                                                put_str_skipped(tip_x, row_y, (*cmd_list)[cmd_idx].tooltip, cb_subtext, row_bg, tip_max_w, tip_skip);
                                        }
                                    }
                                }

                                // VSB: last column, rows dlg_y+2 to dlg_y+2+list_rows-1.
                                // Space is always reserved; track+thumb rendered only when list is scrollable.
                                {
                                    auto list_top_y = dlg_y + 2;
                                    auto n_total    = (si32)filtered.size();
                                    if (n_total > command_bar::max_items && n_visible > 0)
                                    {
                                        auto vsb_mark = (dragging_vsb || hover_vsb) ? "\xe2\x96\x88"  // U+2588
                                                                                     : "\xe2\x96\x90"; // U+2590
                                        // Track (extends into HSB row corner cell).
                                        parent_canvas.fill(rect{{ vsb_x, list_top_y }, { 1, list_rows + 1 }}, [=, ovl_id = ovl_id](cell& c)
                                        {
                                            c.bgc(cb_bg).fgc(cb_vsb_track).txt(vsb_mark).link(ovl_id);
                                        });
                                        // Thumb.
                                        auto track_h   = list_rows + 1; // Full track including corner cell.
                                        auto thumb_h   = std::max(1, track_h * n_visible / n_total);
                                        auto max_vs    = n_total - command_bar::max_items;
                                        auto thumb_off = max_vs > 0 ? v_scroll_off * (track_h - thumb_h) / max_vs : 0;
                                        auto thumb_y   = list_top_y + thumb_off;
                                        auto thumb_fg  = dragging_vsb ? cb_vsb_thumb_drag
                                                       : hover_vsb    ? cb_vsb_thumb_hover
                                                       :                 cb_vsb_thumb;
                                        parent_canvas.fill(rect{{ vsb_x, thumb_y }, { 1, thumb_h }}, [=, ovl_id = ovl_id](cell& c)
                                        {
                                            c.bgc(cb_bg).fgc(thumb_fg).txt(vsb_mark).link(ovl_id);
                                        });
                                    }
                                    // else: dialog background fill already covers the column with cb_bg.
                                }

                                // HSB: bottom row, always reserved.
                                // Track+thumb rendered when list content overflows; otherwise cb_bg blank (list background color).
                                {
                                    auto hsb_y = dlg_y + dlg_h - 1;
                                    if (has_hsb)
                                    {
                                        auto track_w = dlg_w - 1; // cols dlg_x..vsb_x-1; bottom-right cell left blank
                                        auto thumb_w = std::max(1, track_w * list_disp_w / list_content_w);
                                        auto thumb_x = dlg_x + (max_list_hs > 0 ? list_h_scroll_off * (track_w - thumb_w) / max_list_hs : 0);
                                        auto hsb_mark = (dragging_hsb || hover_hsb) ? "\xe2\x96\x84"  // U+2584
                                                                                     : "\xe2\x96\x82"; // U+2582
                                        // Track.
                                        parent_canvas.fill(rect{{ dlg_x, hsb_y }, { track_w, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                        {
                                            c.bgc(cb_bg).fgc(cb_vsb_track).txt(hsb_mark).link(ovl_id);
                                        });
                                        // Thumb.
                                        auto thumb_fg = dragging_hsb ? cb_vsb_thumb_drag
                                                      : hover_hsb    ? cb_vsb_thumb_hover
                                                      :                 cb_vsb_thumb;
                                        parent_canvas.fill(rect{{ thumb_x, hsb_y }, { thumb_w, 1 }}, [=, ovl_id = ovl_id](cell& c)
                                        {
                                            c.bgc(cb_bg).fgc(thumb_fg).txt(hsb_mark).link(ovl_id);
                                        });
                                    }
                                    // else: dialog background fill already covers hsb_y row with cb_bg (list background color).
                                }
                            };

                            // MouseMove: update hover state.
                            ovl.on(tier::mouserelease, input::key::MouseMove,
                                [cmd_list, query_ptr, v_scroll_off_ptr,
                                 hover_row_ptr, hover_vsb_ptr, hover_hsb_ptr,
                                 hover_btn_ptr, cmd_flags,
                                 list_h_scroll_off_ptr, h_scroll_off_ptr,
                                 sel_idx_ptr, kbd_lock_coord_ptr, popup_ready_ptr,
                                 overlay_shadow](hids& gear)
                            {
                                // Initialization gate: discard the very first MouseMove (which reflects
                                // the cursor position before the command bar opened) so that a pre-resting
                                // cursor never highlights an item on entry. Lock kbd_lock_coord_ptr to
                                // that coord so a stationary cursor also stays suppressed.
                                if (!*popup_ready_ptr)
                                {
                                    *popup_ready_ptr = true;
                                    *kbd_lock_coord_ptr = gear.coord;
                                    return;
                                }
                                auto ovl_ptr = overlay_shadow.lock();
                                if (!ovl_ptr) return;
                                // Keyboard priority: while the mouse cursor remains at the exact cell
                                // it occupied when the last keyboard action fired, treat this MouseMove
                                // as noise and skip all hover-derived updates. As soon as the cursor
                                // lands on a different cell we drop the lock and process normally.
                                if (gear.coord == *kbd_lock_coord_ptr) return;
                                *kbd_lock_coord_ptr = twod{ -32768, -32768 };

                                auto& query = *query_ptr;
                                auto data = command_bar::build_model(*cmd_list, query);
                                auto l = command_bar::layout_of(ovl_ptr->base::area().size,
                                                                 data,
                                                                 (si32)utf::length(query),
                                                                 *v_scroll_off_ptr,
                                                                 *h_scroll_off_ptr,
                                                                 *list_h_scroll_off_ptr);
                                if (!l.ok) return;
                                auto mx = (si32)gear.coord.x;
                                auto my = (si32)gear.coord.y;

                                auto new_hover_row = si32{ -1 };
                                auto new_hover_vsb = faux;
                                auto new_hover_hsb = faux;
                                auto new_hover_btn = si32{ 0 };

                                // Pickapp Enter-mode buttons: hit-test the right-anchored
                                // 12-cell strip on the input row.  Only meaningful when the
                                // session opted into both split and replace shortcuts.
                                auto pickapp_caps = (cmd_flags & command_bar::flags::allow_split)
                                                 && (cmd_flags & command_bar::flags::allow_replace);
                                auto enter_btns_show = pickapp_caps
                                                    && l.entry_disp_w >= 4 + 13;
                                if (enter_btns_show && my == l.dlg_y)
                                {
                                    auto x0 = l.dlg_x + l.dlg_w - 1 - 12;
                                    auto rel = mx - x0;
                                    if      (rel >= 0 && rel < 3)  new_hover_btn = 1; // [+]
                                    else if (rel >= 3 && rel < 6)  new_hover_btn = 2; // [⬒]
                                    else if (rel >= 6 && rel < 9)  new_hover_btn = 3; // [|]
                                    else if (rel >= 9 && rel < 12) new_hover_btn = 4; // [-]
                                }

                                if (mx == l.vsb_x && my >= l.dlg_y + 2 && my <= l.dlg_y + 2 + l.list_rows)
                                {
                                    new_hover_vsb = !data.filtered.empty() && (si32)data.filtered.size() > command_bar::max_items;
                                }
                                else if (l.has_hsb && my == l.dlg_y + l.dlg_h - 1 && mx >= l.dlg_x && mx < l.vsb_x)
                                {
                                    new_hover_hsb = true;
                                }
                                else if (!data.filtered.empty()
                                      && my >= l.dlg_y + 2 && my < l.dlg_y + 2 + l.n_visible
                                      && mx >= l.dlg_x && mx < l.vsb_x)
                                {
                                    auto vi = my - (l.dlg_y + 2);
                                    auto fi = vi + l.v_scroll;
                                    if (fi >= 0 && fi < (si32)data.filtered.size())
                                        new_hover_row = fi;
                                }

                                auto changed = (new_hover_row != *hover_row_ptr)
                                            || (new_hover_vsb != *hover_vsb_ptr)
                                            || (new_hover_hsb != *hover_hsb_ptr)
                                            || (new_hover_btn != *hover_btn_ptr);
                                *hover_row_ptr = new_hover_row;
                                *hover_vsb_ptr = new_hover_vsb;
                                *hover_hsb_ptr = new_hover_hsb;
                                *hover_btn_ptr = new_hover_btn;
                                // Update the unified active row when mouse genuinely moves to a list item.
                                // (The kbd-lock guard above ensures stationary cursors cannot trigger this.)
                                if (new_hover_row >= 0 && new_hover_row != *sel_idx_ptr)
                                {
                                    *sel_idx_ptr = new_hover_row;
                                    changed = true;
                                }
                                if (changed) ovl_ptr->base::deface();
                            });

                            // MouseLeave: clear all hover state.
                            ovl.on(tier::mouserelease, input::key::MouseLeave,
                                [hover_row_ptr, hover_vsb_ptr, hover_hsb_ptr, hover_btn_ptr, overlay_shadow](hids& /*gear*/)
                            {
                                auto changed = (*hover_row_ptr >= 0) || *hover_vsb_ptr || *hover_hsb_ptr || (*hover_btn_ptr != 0);
                                *hover_row_ptr = -1;
                                *hover_vsb_ptr = faux;
                                *hover_hsb_ptr = faux;
                                *hover_btn_ptr = 0;
                                if (changed)
                                    if (auto p = overlay_shadow.lock()) p->base::deface();
                            });

                            // MouseWheel: scroll the command list.
                            ovl.on(tier::mouserelease, input::key::MouseWheel,
                                [cmd_list, query_ptr, v_scroll_off_ptr, overlay_shadow](hids& gear)
                            {
                                auto ovl_ptr = overlay_shadow.lock();
                                if (!ovl_ptr) return;
                                auto data = command_bar::build_model(*cmd_list, *query_ptr);
                                auto delta  = gear.whlsi < 0 ? 1 : -1;
                                auto max_vs = std::max(0, (si32)data.filtered.size() - command_bar::max_items);
                                *v_scroll_off_ptr = std::clamp(*v_scroll_off_ptr + delta, 0, max_vs);
                                ovl_ptr->base::deface();
                                gear.dismiss();
                            });

                            // LeftClick: execute selected command, scroll via track area, or dismiss.
                            ovl.on(tier::mouserelease, input::key::LeftClick,
                                [cmd_list, query_ptr, v_scroll_off_ptr, h_scroll_off_ptr, list_h_scroll_off_ptr,
                                 hover_row_ptr, dragging_vsb_ptr, dragging_hsb_ptr,
                                 dismiss_visual, dismiss_hook, overlay_shadow,
                                 dispatch_script,
                                 enter_mode_ptr, cmd_flags,
                                 boss_shadow](hids& gear)
                            {
                                if (*dragging_vsb_ptr || *dragging_hsb_ptr) return; // Drag already handled.
                                auto ovl_ptr = overlay_shadow.lock();
                                if (!ovl_ptr) return;
                                auto& query = *query_ptr;
                                auto data = command_bar::build_model(*cmd_list, query);
                                auto l = command_bar::layout_of(ovl_ptr->base::area().size,
                                                                 data,
                                                                 (si32)utf::length(query),
                                                                 *v_scroll_off_ptr,
                                                                 *h_scroll_off_ptr,
                                                                 *list_h_scroll_off_ptr);
                                if (!l.ok) return;

                                auto& filtered = data.filtered;
                                auto n_total = l.n_total;
                                auto n_visible = l.n_visible;
                                auto dlg_w = l.dlg_w;
                                auto dlg_h = l.dlg_h;
                                auto dlg_x = l.dlg_x;
                                auto dlg_y = l.dlg_y;
                                auto vsb_x = l.vsb_x;
                                auto list_disp_w = l.list_disp_w;
                                auto list_content_w = l.list_content_w;
                                auto has_hsb = l.has_hsb;
                                auto list_rows = l.list_rows;
                                auto v_scroll_off = l.v_scroll;
                                auto list_h_scroll_off = l.list_h_scroll;
                                auto mx = (si32)gear.coord.x;
                                auto my = (si32)gear.coord.y;

                                // Click on a pickapp Enter-mode button — toggle radio state.
                                // Clicking the already-active button cancels it (back to
                                // enter_mode_none).  Buttons sit on the input row only and
                                // never dismiss the overlay.
                                auto pickapp_caps = (cmd_flags & command_bar::flags::allow_split)
                                                 && (cmd_flags & command_bar::flags::allow_replace);
                                auto enter_btns_show = pickapp_caps && l.entry_disp_w >= 4 + 13;
                                if (enter_btns_show && my == dlg_y)
                                {
                                    auto x0 = dlg_x + dlg_w - 1 - 12;
                                    auto rel = mx - x0;
                                    auto clicked = si32{ 0 };
                                    if      (rel >= 0 && rel < 3)  clicked = 1; // [+]
                                    else if (rel >= 3 && rel < 6)  clicked = 2; // [⬒]
                                    else if (rel >= 6 && rel < 9)  clicked = 3; // [|]
                                    else if (rel >= 9 && rel < 12) clicked = 4; // [-]
                                    if (clicked)
                                    {
                                        *enter_mode_ptr = (*enter_mode_ptr == clicked) ? 0 : clicked;
                                        ovl_ptr->base::deface();
                                        gear.dismiss();
                                        return;
                                    }
                                }

                                // Click on a list item — execute its script.
                                if (!filtered.empty()
                                 && my >= dlg_y + 2 && my < dlg_y + 2 + n_visible
                                 && mx >= dlg_x && mx < vsb_x)
                                {
                                    auto vi = my - (dlg_y + 2);
                                    auto fi = vi + v_scroll_off;
                                    if (fi >= 0 && fi < n_total)
                                    {
                                        auto cmd_idx = filtered[(size_t)fi];
                                        auto script = (*cmd_list)[cmd_idx].script;
                                        auto mode = *enter_mode_ptr;
                                        if (mode == 1 && (cmd_flags & command_bar::flags::allow_replace))
                                        {
                                            script += "\nvtm.tile.ReRunApplication();";
                                        }
                                        else if (mode == 2)
                                        {
                                            script += "\nvtm.tile.CreateWorkspace();";
                                        }
                                        else if (mode == 3 && (cmd_flags & command_bar::flags::allow_split))
                                        {
                                            script += "\nvtm.tile.SplitPane(0);";
                                        }
                                        else if (mode == 4 && (cmd_flags & command_bar::flags::allow_split))
                                        {
                                            script += "\nvtm.tile.SplitPane(1);";
                                        }
                                        dismiss_visual();
                                        dismiss_hook();
                                        dispatch_script(script, gear);
                                    }
                                    gear.dismiss();
                                    return;
                                }

                                // Click on VSB track area — page up or page down.
                                if (mx == vsb_x && n_total > command_bar::max_items && n_visible > 0
                                 && my >= dlg_y + 2 && my <= dlg_y + 2 + list_rows)
                                {
                                    auto track_h   = list_rows + 1; // Full track including corner cell.
                                    auto thumb_h   = std::max(1, track_h * n_visible / n_total);
                                    auto max_vs    = n_total - command_bar::max_items;
                                    auto thumb_off = max_vs > 0 ? v_scroll_off * (track_h - thumb_h) / max_vs : 0;
                                    auto thumb_y   = dlg_y + 2 + thumb_off;
                                    if (my < thumb_y)
                                        *v_scroll_off_ptr = std::clamp(v_scroll_off - command_bar::max_items, 0, max_vs);
                                    else if (my >= thumb_y + thumb_h)
                                        *v_scroll_off_ptr = std::clamp(v_scroll_off + command_bar::max_items, 0, max_vs);
                                    ovl_ptr->base::deface();
                                    gear.dismiss();
                                    return;
                                }

                                // Click on HSB track area — page left or page right.
                                if (has_hsb && my == dlg_y + dlg_h - 1 && mx >= dlg_x && mx < vsb_x)
                                {
                                    auto track_w = dlg_w - 1;
                                    auto max_hs  = list_content_w - list_disp_w;
                                    auto thumb_w = std::max(1, track_w * list_disp_w / list_content_w);
                                    auto h_off   = list_h_scroll_off;
                                    auto thumb_x = dlg_x + (max_hs > 0 ? h_off * (track_w - thumb_w) / max_hs : 0);
                                    if (mx < thumb_x)
                                        *list_h_scroll_off_ptr = std::clamp(h_off - list_disp_w, 0, max_hs);
                                    else if (mx >= thumb_x + thumb_w)
                                        *list_h_scroll_off_ptr = std::clamp(h_off + list_disp_w, 0, max_hs);
                                    ovl_ptr->base::deface();
                                    gear.dismiss();
                                    return;
                                }

                                // Click outside dialog — dismiss.
                                if (my < dlg_y || my >= dlg_y + dlg_h || mx < dlg_x || mx >= dlg_x + dlg_w)
                                {
                                    dismiss_visual();
                                    dismiss_hook();
                                    gear.dismiss();
                                }
                            });

                            // LeftDragStart: begin VSB or HSB drag.
                            ovl.on(tier::mouserelease, input::key::LeftDragStart,
                                [cmd_list, query_ptr, v_scroll_off_ptr, h_scroll_off_ptr, list_h_scroll_off_ptr,
                                 hover_vsb_ptr, hover_hsb_ptr,
                                 dragging_vsb_ptr, dragging_hsb_ptr,
                                 drag_vsb_grab_ptr, drag_hsb_grab_ptr,
                                 overlay_shadow](hids& gear)
                            {
                                auto ovl_ptr = overlay_shadow.lock();
                                if (!ovl_ptr) return;
                                auto& query = *query_ptr;
                                auto data = command_bar::build_model(*cmd_list, query);
                                auto l = command_bar::layout_of(ovl_ptr->base::area().size,
                                                                 data,
                                                                 (si32)utf::length(query),
                                                                 *v_scroll_off_ptr,
                                                                 *h_scroll_off_ptr,
                                                                 *list_h_scroll_off_ptr);
                                if (!l.ok) return;

                                auto n_visible      = l.n_visible;
                                auto dlg_w          = l.dlg_w;
                                auto list_disp_w    = l.list_disp_w;
                                auto list_content_w = l.list_content_w;
                                auto has_hsb        = l.has_hsb;
                                auto list_rows      = l.list_rows;
                                auto dlg_h          = l.dlg_h;
                                auto dlg_x          = l.dlg_x;
                                auto dlg_y          = l.dlg_y;
                                auto vsb_x          = l.vsb_x;
                                auto n_total        = l.n_total;
                                auto v_scroll_off   = l.v_scroll;
                                auto list_h_scroll_off = l.list_h_scroll;
                                auto mx             = (si32)gear.pressxy.x;
                                auto my             = (si32)gear.pressxy.y;

                                // VSB drag start.
                                if (mx == vsb_x && n_total > command_bar::max_items && n_visible > 0
                                 && my >= dlg_y + 2 && my <= dlg_y + 2 + list_rows)
                                {
                                    auto track_h   = list_rows + 1; // Full track including corner cell.
                                    auto thumb_h   = std::max(1, track_h * n_visible / n_total);
                                    auto max_vs    = n_total - command_bar::max_items;
                                    auto thumb_off = max_vs > 0 ? v_scroll_off * (track_h - thumb_h) / max_vs : 0;
                                    auto thumb_y   = dlg_y + 2 + thumb_off;
                                    if (my >= thumb_y && my < thumb_y + thumb_h)
                                    {
                                        *drag_vsb_grab_ptr = my - thumb_y;
                                    }
                                    else
                                    {
                                        // Snap thumb center to click.
                                        *drag_vsb_grab_ptr = thumb_h / 2;
                                        auto new_thumb_y   = my - *drag_vsb_grab_ptr - (dlg_y + 2);
                                        auto new_vs        = (track_h > thumb_h && max_vs > 0)
                                                           ? new_thumb_y * max_vs / (track_h - thumb_h) : 0;
                                        *v_scroll_off_ptr  = std::clamp(new_vs, 0, max_vs);
                                    }
                                    *dragging_vsb_ptr = true;
                                    *hover_vsb_ptr    = true;
                                    ovl_ptr->base::deface();
                                    gear.dismiss();
                                    return;
                                }

                                // HSB drag start.
                                if (has_hsb && my == dlg_y + dlg_h - 1 && mx >= dlg_x && mx < vsb_x)
                                {
                                    auto track_w = dlg_w - 1;
                                    auto max_hs  = list_content_w - list_disp_w;
                                    auto thumb_w = std::max(1, track_w * list_disp_w / list_content_w);
                                    auto h_off   = list_h_scroll_off;
                                    auto thumb_x = dlg_x + (max_hs > 0 ? h_off * (track_w - thumb_w) / max_hs : 0);
                                    if (mx >= thumb_x && mx < thumb_x + thumb_w)
                                    {
                                        *drag_hsb_grab_ptr = mx - thumb_x;
                                    }
                                    else
                                    {
                                        // Snap thumb center to click.
                                        *drag_hsb_grab_ptr = thumb_w / 2;
                                        auto new_thumb_x   = mx - *drag_hsb_grab_ptr - dlg_x;
                                        auto new_hs_val    = (track_w > thumb_w && max_hs > 0)
                                                           ? new_thumb_x * max_hs / (track_w - thumb_w) : 0;
                                        *list_h_scroll_off_ptr = std::clamp(new_hs_val, 0, max_hs);
                                    }
                                    *dragging_hsb_ptr = true;
                                    *hover_hsb_ptr    = true;
                                    ovl_ptr->base::deface();
                                    gear.dismiss();
                                    return;
                                }
                            });

                            // LeftDragPull: update scroll offset while dragging.
                            ovl.on(tier::mouserelease, input::key::LeftDragPull,
                                [cmd_list, query_ptr, v_scroll_off_ptr, h_scroll_off_ptr, list_h_scroll_off_ptr,
                                 dragging_vsb_ptr, dragging_hsb_ptr,
                                 drag_vsb_grab_ptr, drag_hsb_grab_ptr,
                                 overlay_shadow](hids& gear)
                            {
                                if (!*dragging_vsb_ptr && !*dragging_hsb_ptr) return;
                                auto ovl_ptr = overlay_shadow.lock();
                                if (!ovl_ptr) return;
                                auto& query = *query_ptr;
                                auto data = command_bar::build_model(*cmd_list, query);
                                auto l = command_bar::layout_of(ovl_ptr->base::area().size,
                                                                 data,
                                                                 (si32)utf::length(query),
                                                                 *v_scroll_off_ptr,
                                                                 *h_scroll_off_ptr,
                                                                 *list_h_scroll_off_ptr);
                                if (!l.ok) return;

                                auto n_visible      = l.n_visible;
                                auto dlg_w          = l.dlg_w;
                                auto list_disp_w    = l.list_disp_w;
                                auto list_content_w = l.list_content_w;
                                auto n_total        = l.n_total;
                                auto mx             = (si32)gear.coord.x;
                                auto my             = (si32)gear.coord.y;

                                if (*dragging_vsb_ptr)
                                {
                                    auto max_vs     = n_total - command_bar::max_items;
                                    if (max_vs <= 0) return;
                                    auto list_rows   = n_visible; // n_visible == max_items when dragging
                                    auto track_h     = list_rows + 1; // Full track including corner cell.
                                    auto thumb_h     = std::max(1, track_h * n_visible / n_total);
                                    auto dlg_y       = l.dlg_y;
                                    auto new_thumb_y = my - *drag_vsb_grab_ptr - (dlg_y + 2);
                                    auto new_vs      = (track_h > thumb_h)
                                                     ? new_thumb_y * max_vs / (track_h - thumb_h) : 0;
                                    *v_scroll_off_ptr = std::clamp(new_vs, 0, max_vs);
                                    ovl_ptr->base::deface();
                                    gear.dismiss();
                                    return;
                                }

                                if (*dragging_hsb_ptr)
                                {
                                    auto dlg_x       = l.dlg_x;
                                    auto track_w     = dlg_w - 1;
                                    auto max_hs      = std::max(0, list_content_w - list_disp_w);
                                    auto thumb_w     = std::max(1, track_w * list_disp_w / std::max(1, list_content_w));
                                    auto new_thumb_x = mx - *drag_hsb_grab_ptr - dlg_x;
                                    auto new_hs_val  = (track_w > thumb_w && max_hs > 0)
                                                     ? new_thumb_x * max_hs / (track_w - thumb_w) : 0;
                                    *list_h_scroll_off_ptr = std::clamp(new_hs_val, 0, max_hs);
                                    ovl_ptr->base::deface();
                                    gear.dismiss();
                                    return;
                                }
                            });

                            // LeftDragStop: end drag.
                            ovl.on(tier::mouserelease, input::key::LeftDragStop,
                                [dragging_vsb_ptr, dragging_hsb_ptr,
                                 hover_vsb_ptr, hover_hsb_ptr, overlay_shadow](hids& gear)
                            {
                                if (!*dragging_vsb_ptr && !*dragging_hsb_ptr) return;
                                *dragging_vsb_ptr = faux;
                                *dragging_hsb_ptr = faux;
                                if (auto p = overlay_shadow.lock()) p->base::deface();
                                gear.dismiss();
                            });

                            // LeftDragCancel: cancel drag.
                            ovl.on(tier::mouserelease, input::key::LeftDragCancel,
                                [dragging_vsb_ptr, dragging_hsb_ptr,
                                 hover_vsb_ptr, hover_hsb_ptr, overlay_shadow](hids& gear)
                            {
                                if (!*dragging_vsb_ptr && !*dragging_hsb_ptr) return;
                                *dragging_vsb_ptr = faux;
                                *dragging_hsb_ptr = faux;
                                *hover_vsb_ptr    = faux;
                                *hover_hsb_ptr    = faux;
                                if (auto p = overlay_shadow.lock()) p->base::deface();
                                gear.dismiss();
                            });
                        });
                        wrapper_ptr->attach(overlay_ptr);
                        wrapper_ptr->base::reflow();
                        wrapper_ptr->base::deface();

                        // Keyboard interceptor.
                        wrapper_ptr->bell::submit(tier::preview, input::events::keybd::any, *kbd_hook)
                            = [cmd_list, query_ptr, caret_cp_ptr, sel_idx_ptr,
                               v_scroll_off_ptr, h_scroll_off_ptr, list_h_scroll_off_ptr,
                               kbd_lock_coord_ptr,
                               overlay_shadow, boss_shadow,
                               dispatch_script, cmd_flags,
                               enter_mode_ptr,
                               dismiss_visual, dismiss_hook, pending_unhook](hids& gear) mutable
                        {
                            if (gear.payload != input::keybd::type::keypress
                             && gear.payload != input::keybd::type::keypaste) return;
                            if (gear.payload == input::keybd::type::keypress
                             && gear.keystat == input::key::interrupted)      return;
                            if (gear.keybd::handled)                          return;

                            // After visual dismiss: swallow trailing events until key release.
                            if (*pending_unhook)
                            {
                                if (gear.payload == input::keybd::type::keypress
                                 && gear.keystat == input::key::released) dismiss_hook();
                                gear.set_handled(faux);
                                return;
                            }

                            // Swallow key-release events.
                            if (gear.payload == input::keybd::type::keypress
                             && gear.keystat == input::key::released)
                            {
                                gear.set_handled(faux);
                                return;
                            }

                            auto ovl_ptr = overlay_shadow.lock();

                            // Helper: rebuild the filtered index list.
                            auto build_filtered = [&]() -> std::vector<si32>
                            {
                                return command_bar::build_model(*cmd_list, *query_ptr).filtered;
                            };

                            // Helper: compute entry_disp_w from overlay area.
                            // In pickapp sessions the input row hosts a 9-cell button
                            // strip on its right side; whenever the strip is wide enough
                            // to coexist with at least 4 input cells we subtract those
                            // 9 cells so caret-tracking and h-scroll honor the visible
                            // input width.  The threshold mirrors the render path's
                            // responsive-degradation rule.
                            auto get_entry_disp_w = [&]() -> si32
                            {
                                if (!ovl_ptr) return 60; // Fallback.
                                auto full_w = ovl_ptr->base::area().size.x;
                                auto dw = std::min(command_bar::dlg_w_max, full_w - 4);
                                auto edw = std::max(1, dw - 4);
                                auto pickapp_caps = (cmd_flags & command_bar::flags::allow_split)
                                                 && (cmd_flags & command_bar::flags::allow_replace);
                                if (pickapp_caps && edw >= 4 + 13) edw -= 13;
                                return edw;
                            };

                            auto keep_caret_visible = [&]
                            {
                                auto& q = *query_ptr;
                                auto query_len = command_bar::cp_len(q);
                                *caret_cp_ptr = std::clamp(*caret_cp_ptr, si32{ 0 }, query_len);
                                auto edw = get_entry_disp_w();
                                if (*h_scroll_off_ptr > *caret_cp_ptr)
                                {
                                    *h_scroll_off_ptr = *caret_cp_ptr;
                                }
                                if (*caret_cp_ptr - *h_scroll_off_ptr >= edw)
                                {
                                    *h_scroll_off_ptr = *caret_cp_ptr - edw + 1;
                                }
                                auto max_hs = std::max(si32{ 0 }, query_len - edw + 1);
                                *h_scroll_off_ptr = std::clamp(*h_scroll_off_ptr, si32{ 0 }, max_hs);
                            };

                            auto keep_selection_valid = [&]
                            {
                                auto filtered = build_filtered();
                                if (*sel_idx_ptr >= (si32)filtered.size())
                                {
                                    *sel_idx_ptr = filtered.empty() ? 0 : (si32)filtered.size() - 1;
                                }
                                auto max_vs = std::max(si32{ 0 }, (si32)filtered.size() - command_bar::max_items);
                                *v_scroll_off_ptr = std::clamp(*v_scroll_off_ptr, si32{ 0 }, max_vs);
                            };

                            auto reset_result_scroll = [&]
                            {
                                *sel_idx_ptr = 0;
                                *v_scroll_off_ptr = 0;
                                *list_h_scroll_off_ptr = 0;
                            };

                            auto refresh_query = [&]
                            {
                                keep_selection_valid();
                                keep_caret_visible();
                            };

                            auto insert_query_text = [&](view src) -> bool
                            {
                                auto buf = command_bar::filter_input_text(src);
                                if (buf.empty()) return faux;
                                auto& q = *query_ptr;
                                auto query_len = command_bar::cp_len(q);
                                *caret_cp_ptr = std::clamp(*caret_cp_ptr, si32{ 0 }, query_len);
                                auto pos = command_bar::byte_of_cp(q, *caret_cp_ptr);
                                q.insert(pos, buf);
                                *caret_cp_ptr += command_bar::cp_len(buf);
                                reset_result_scroll();
                                refresh_query();
                                return true;
                            };

                            if (gear.payload == input::keybd::type::keypaste)
                            {
                                if (insert_query_text(gear.cluster))
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            auto k = gear.keybd::generic();
                            auto ctrl  = !!(gear.ctlstat & hids::anyCtrl);
                            auto alt   = !!(gear.ctlstat & hids::anyAlt);
                            auto shift = !!(gear.ctlstat & hids::anyShift);

                            auto move_selection = [&](si32 delta)
                            {
                                auto filtered = build_filtered();
                                if (!filtered.empty())
                                {
                                    auto next = std::clamp(*sel_idx_ptr + delta, si32{ 0 }, (si32)filtered.size() - 1);
                                    if (next != *sel_idx_ptr)
                                    {
                                        *sel_idx_ptr = next;
                                        if (*sel_idx_ptr < *v_scroll_off_ptr)
                                        {
                                            *v_scroll_off_ptr = *sel_idx_ptr;
                                        }
                                        auto vis_end = *v_scroll_off_ptr + command_bar::max_items - 1;
                                        if (*sel_idx_ptr > vis_end)
                                        {
                                            *v_scroll_off_ptr = *sel_idx_ptr - command_bar::max_items + 1;
                                        }
                                    }
                                }
                                *kbd_lock_coord_ptr = gear.coord; // Lock out echo MouseMove at this coord.
                                if (ovl_ptr) ovl_ptr->base::deface();
                            };

                            auto total_cp = [&] { return command_bar::cp_len(*query_ptr); };
                            auto is_ws_cp = [&](si32 cp) -> bool
                            {
                                auto total = total_cp();
                                if (cp < 0 || cp >= total) return true;
                                auto i = command_bar::byte_of_cp(*query_ptr, cp);
                                auto c = (unsigned char)(*query_ptr)[i];
                                return c == 0x20 || c == 0x09;
                            };
                            auto prev_word_cp = [&](si32 cp) -> si32
                            {
                                if (cp <= 0) return 0;
                                while (cp > 0 && is_ws_cp(cp - 1)) --cp;
                                while (cp > 0 && !is_ws_cp(cp - 1)) --cp;
                                return cp;
                            };
                            auto next_word_cp = [&](si32 cp) -> si32
                            {
                                auto total = total_cp();
                                if (cp >= total) return total;
                                while (cp < total && is_ws_cp(cp)) ++cp;
                                while (cp < total && !is_ws_cp(cp)) ++cp;
                                return cp;
                            };
                            auto erase_cp_range = [&](si32 from_cp, si32 to_cp) -> bool
                            {
                                auto& q = *query_ptr;
                                auto total = total_cp();
                                from_cp = std::clamp(from_cp, si32{ 0 }, total);
                                to_cp   = std::clamp(to_cp,   si32{ 0 }, total);
                                if (from_cp >= to_cp) return faux;
                                auto a = command_bar::byte_of_cp(q, from_cp);
                                auto b = command_bar::byte_of_cp(q, to_cp);
                                q.erase(a, b - a);
                                if (*caret_cp_ptr > to_cp)        *caret_cp_ptr -= to_cp - from_cp;
                                else if (*caret_cp_ptr > from_cp) *caret_cp_ptr  = from_cp;
                                reset_result_scroll();
                                refresh_query();
                                return true;
                            };

                            // Esc / Alt+L — dismiss.
                            if (k == input::key::Esc
                             || (alt && !ctrl && k == input::key::KeyL))
                            {
                                dismiss_visual();
                                gear.set_handled(faux);
                                return;
                            }

                            // Tab / Shift+Tab — cycle enter-mode buttons ([+]/[⬒]/[|]/[-]/none).
                            // Only active in pickapp context (both allow_split and allow_replace
                            // must be set).  No Ctrl/Alt modifier accepted to avoid stealing
                            // common terminal shortcuts.
                            if (!ctrl && !alt && k == input::key::Tab
                             && (cmd_flags & command_bar::flags::allow_split)
                             && (cmd_flags & command_bar::flags::allow_replace))
                            {
                                auto& mode = *enter_mode_ptr;
                                if (shift)
                                    mode = (mode == 0) ? 4 : mode - 1; // 0→4→3→2→1→0
                                else
                                    mode = (mode + 1) % 5;             // 0→1→2→3→4→0
                                if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            // Enter — execute selected command and dismiss.  When a
                            // pickapp Enter-mode button ([+]/[⬒]/[|]/[-]) is currently
                            // selected, postfix the corresponding tile script so plain
                            // Enter mirrors the Ctrl+R / Ctrl+V / Ctrl+S shortcuts that
                            // would have produced the same effect via keyboard alone.
                            // The mode is honored only when the session is also
                            // entitled to the underlying shortcut (allow_split for the
                            // split modes, allow_replace for rerun); otherwise the mode
                            // is silently ignored, matching the existing shortcut gates.
                            if (k == input::key::KeyEnter || k == input::key::NumpadEnter)
                            {
                                auto filtered = build_filtered();
                                if (!filtered.empty() && *sel_idx_ptr < (si32)filtered.size())
                                {
                                    auto cmd_idx = filtered[(size_t)*sel_idx_ptr];
                                    auto script = (*cmd_list)[cmd_idx].script;
                                    auto mode = *enter_mode_ptr;
                                    if (mode == 1 && (cmd_flags & command_bar::flags::allow_replace))
                                    {
                                        script += "\nvtm.tile.ReRunApplication();";
                                    }
                                    else if (mode == 2)
                                    {
                                        script += "\nvtm.tile.CreateWorkspace();";
                                    }
                                    else if (mode == 3 && (cmd_flags & command_bar::flags::allow_split))
                                    {
                                        script += "\nvtm.tile.SplitPane(0);";
                                    }
                                    else if (mode == 4 && (cmd_flags & command_bar::flags::allow_split))
                                    {
                                        script += "\nvtm.tile.SplitPane(1);";
                                    }
                                    dismiss_visual();
                                    dismiss_hook();
                                    dispatch_script(script, gear);
                                }
                                else
                                {
                                    dismiss_visual();
                                    dismiss_hook();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            // Ctrl+V — execute selected command and split pane vertically (left/right).
                            if (ctrl && !alt && k == input::key::KeyV
                             && (cmd_flags & command_bar::flags::allow_split))
                            {
                                auto filtered = build_filtered();
                                if (!filtered.empty() && *sel_idx_ptr < (si32)filtered.size())
                                {
                                    auto cmd_idx = filtered[(size_t)*sel_idx_ptr];
                                    auto script = (*cmd_list)[cmd_idx].script + "\nvtm.tile.SplitPane(0);";
                                    dismiss_visual();
                                    dismiss_hook();
                                    dispatch_script(script, gear);
                                }
                                else
                                {
                                    dismiss_visual();
                                    dismiss_hook();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            // Ctrl+S — execute selected command and split pane horizontally (top/bottom).
                            if (ctrl && !alt && k == input::key::KeyS
                             && (cmd_flags & command_bar::flags::allow_split))
                            {
                                auto filtered = build_filtered();
                                if (!filtered.empty() && *sel_idx_ptr < (si32)filtered.size())
                                {
                                    auto cmd_idx = filtered[(size_t)*sel_idx_ptr];
                                    auto script = (*cmd_list)[cmd_idx].script + "\nvtm.tile.SplitPane(1);";
                                    dismiss_visual();
                                    dismiss_hook();
                                    dispatch_script(script, gear);
                                }
                                else
                                {
                                    dismiss_visual();
                                    dismiss_hook();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            // Ctrl+R — execute selected command, close current pane, then run application.
                            if (ctrl && !alt && k == input::key::KeyR
                             && (cmd_flags & command_bar::flags::allow_replace))
                            {
                                auto filtered = build_filtered();
                                if (!filtered.empty() && *sel_idx_ptr < (si32)filtered.size())
                                {
                                    auto cmd_idx = filtered[(size_t)*sel_idx_ptr];
                                    auto script = (*cmd_list)[cmd_idx].script
                                                + "\nvtm.tile.ReRunApplication();";
                                    dismiss_visual();
                                    dismiss_hook();
                                    dispatch_script(script, gear);
                                }
                                else
                                {
                                    dismiss_visual();
                                    dismiss_hook();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            // Ctrl+W — execute selected command and create a new workspace.
                            if (ctrl && !alt && k == input::key::KeyW
                             && (cmd_flags & command_bar::flags::allow_split)
                             && (cmd_flags & command_bar::flags::allow_replace))
                            {
                                auto filtered = build_filtered();
                                if (!filtered.empty() && *sel_idx_ptr < (si32)filtered.size())
                                {
                                    auto cmd_idx = filtered[(size_t)*sel_idx_ptr];
                                    auto script = (*cmd_list)[cmd_idx].script
                                                + "\nvtm.tile.CreateWorkspace();";
                                    dismiss_visual();
                                    dismiss_hook();
                                    dispatch_script(script, gear);
                                }
                                else
                                {
                                    dismiss_visual();
                                    dismiss_hook();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            // Arrow Up / Alt+K — move selection up.
                            if (k == input::key::KeyUpArrow
                             || k == input::key::NumpadUpArrow
                             || (ctrl && !alt && k == input::key::KeyP)
                             || (alt && !ctrl && k == input::key::KeyK))
                            {
                                move_selection(-1);
                                gear.set_handled(faux);
                                return;
                            }

                            // Arrow Down / Alt+J — move selection down.
                            if (k == input::key::KeyDownArrow
                             || k == input::key::NumpadDownArrow
                             || (ctrl && !alt && k == input::key::KeyN)
                             || (alt && !ctrl && k == input::key::KeyJ))
                            {
                                move_selection(1);
                                gear.set_handled(faux);
                                return;
                            }

                            // PageUp — move selection up by one page.
                            if (k == input::key::KeyPageUp || k == input::key::NumpadPageUp)
                            {
                                move_selection(-command_bar::max_items);
                                gear.set_handled(faux);
                                return;
                            }

                            // PageDown — move selection down by one page.
                            if (k == input::key::KeyPageDown || k == input::key::NumpadPageDown)
                            {
                                move_selection(command_bar::max_items);
                                gear.set_handled(faux);
                                return;
                            }

                            // Backspace - delete before caret, or kill word backward with Alt.
                            if (k == input::key::Backspace)
                            {
                                auto changed = alt ? erase_cp_range(prev_word_cp(*caret_cp_ptr), *caret_cp_ptr)
                                                   : erase_cp_range(*caret_cp_ptr - 1, *caret_cp_ptr);
                                if (changed)
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            if (k == input::key::KeyDelete || k == input::key::NumpadDelete)
                            {
                                auto changed = alt ? erase_cp_range(*caret_cp_ptr, next_word_cp(*caret_cp_ptr))
                                                   : erase_cp_range(*caret_cp_ptr, *caret_cp_ptr + 1);
                                if (changed)
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            if (ctrl && !alt && k == input::key::KeyA)
                            {
                                if (*caret_cp_ptr != 0)
                                {
                                    *caret_cp_ptr = 0;
                                    keep_caret_visible();
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            if (ctrl && !alt && k == input::key::KeyE)
                            {
                                auto total = total_cp();
                                if (*caret_cp_ptr != total)
                                {
                                    *caret_cp_ptr = total;
                                    keep_caret_visible();
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            if (ctrl && !alt && k == input::key::KeyB)
                            {
                                if (*caret_cp_ptr > 0)
                                {
                                    --(*caret_cp_ptr);
                                    keep_caret_visible();
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            if (ctrl && !alt && k == input::key::KeyF)
                            {
                                auto total = total_cp();
                                if (*caret_cp_ptr < total)
                                {
                                    ++(*caret_cp_ptr);
                                    keep_caret_visible();
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            if (ctrl && !alt && k == input::key::KeyD)
                            {
                                auto changed = erase_cp_range(*caret_cp_ptr, *caret_cp_ptr + 1);
                                if (changed)
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            if (ctrl && !alt && k == input::key::KeyH)
                            {
                                auto changed = erase_cp_range(*caret_cp_ptr - 1, *caret_cp_ptr);
                                if (changed)
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            if (ctrl && !alt && k == input::key::KeyK)
                            {
                                auto changed = erase_cp_range(*caret_cp_ptr, total_cp());
                                if (changed)
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            if (ctrl && !alt && k == input::key::KeyU)
                            {
                                auto changed = erase_cp_range(0, *caret_cp_ptr);
                                if (changed)
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            if (ctrl && !alt && k == input::key::KeyW)
                            {
                                auto changed = erase_cp_range(prev_word_cp(*caret_cp_ptr), *caret_cp_ptr);
                                if (changed)
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            if (alt && !ctrl && k == input::key::KeyB)
                            {
                                auto next = prev_word_cp(*caret_cp_ptr);
                                if (next != *caret_cp_ptr)
                                {
                                    *caret_cp_ptr = next;
                                    keep_caret_visible();
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            if (alt && !ctrl && k == input::key::KeyF)
                            {
                                auto next = next_word_cp(*caret_cp_ptr);
                                if (next != *caret_cp_ptr)
                                {
                                    *caret_cp_ptr = next;
                                    keep_caret_visible();
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            if (alt && !ctrl && k == input::key::KeyD)
                            {
                                auto changed = erase_cp_range(*caret_cp_ptr, next_word_cp(*caret_cp_ptr));
                                if (changed)
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            if (k == input::key::KeyLeftArrow || k == input::key::NumpadLeftArrow)
                            {
                                if (*caret_cp_ptr > 0)
                                {
                                    --(*caret_cp_ptr);
                                    keep_caret_visible();
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            if (k == input::key::KeyRightArrow || k == input::key::NumpadRightArrow)
                            {
                                auto total = total_cp();
                                if (*caret_cp_ptr < total)
                                {
                                    ++(*caret_cp_ptr);
                                    keep_caret_visible();
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            if (k == input::key::KeyHome || k == input::key::NumpadHome)
                            {
                                if (*caret_cp_ptr != 0)
                                {
                                    *caret_cp_ptr = 0;
                                    keep_caret_visible();
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            if (k == input::key::KeyEnd || k == input::key::NumpadEnd)
                            {
                                auto total = total_cp();
                                if (*caret_cp_ptr != total)
                                {
                                    *caret_cp_ptr = total;
                                    keep_caret_visible();
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                }
                                gear.set_handled(faux);
                                return;
                            }

                            if (ctrl && !alt && k == input::key::KeyY)
                            {
                                gear.owner.base::signal(tier::request, input::events::clipboard, gear);
                                if (insert_query_text(gear.board::cargo.utf8))
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            // Printable character - insert at caret.
                            auto& ch = gear.keybd::cluster;
                            if (!ctrl && !alt && !ch.empty() && (unsigned char)ch[0] >= 0x20)
                            {
                                if (insert_query_text(ch))
                                    if (ovl_ptr) ovl_ptr->base::deface();
                                gear.set_handled(faux);
                                return;
                            }

                            // Ignore all other keys.
                            gear.set_handled(faux);
                        };

                        gear.set_handled();
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::focus::pickapp, gear, -, (command_bar_active, pending_cmd_list_ptr, pending_cmd_flags_ptr))
                    {
                        if (*command_bar_active) { gear.set_handled(); return; }
                        auto data = get_apps_data(boss);
                        if (data.ids.empty()) { gear.set_handled(); return; }
                        // Build a synthetic command-bar item list: one entry per configured app.
                        // The script for each entry sets the selected default app via the
                        // SetSelectedApp Lua method (which signals events::ui::setapp).
                        auto items = ptr::shared(std::vector<command_bar::item>{});
                        items->reserve(data.ids.size());
                        for (auto i = 0u; i < data.ids.size(); ++i)
                        {
                            auto& _id = data.ids[i];
                            auto& lbl = data.labels[i];
                            auto& cmd = data.cmds[i];
                            // Single-quote any embedded single quotes in id to keep the Lua literal safe.
                            auto safe_id = text{};
                            safe_id.reserve(_id.size());
                            for (auto c : _id)
                            {
                                if (c == '\'' || c == '\\') safe_id.push_back('\\');
                                safe_id.push_back(c);
                            }
                            auto display = (i == data.selected_index) ? "* "s + lbl : "  "s + lbl;
                            auto tooltip = cmd;
                            auto script  = "vtm.tile.SetSelectedApp('"s + safe_id + "')";
                            items->push_back({ display, tooltip, script });
                        }
                        *pending_cmd_flags_ptr = command_bar::flags::allow_split
                                               | command_bar::flags::allow_replace;
                        *pending_cmd_list_ptr = items;
                        boss.base::signal(tier::preview, app::tile::events::ui::focus::commandbar, gear);
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::swap, gear, -, (current_focus_history))
                    {
                        auto focus_history_ptr = current_focus_history();
                        if (nothing_to_iterate()) return;
                        auto node_veer_list = std::vector<netxs::sptr<ui::veer>>{};
                        auto node_grip_list = std::vector<netxs::sptr<ui::veer>>{};
                        foreach(gear.id, [&](auto& /*item_ptr*/, si32 item_type, auto node_veer_ptr)
                        {
                            if (item_type == item_type::grip)
                            {
                                node_grip_list.push_back(node_veer_ptr);
                            }
                            else
                            {
                                node_veer_list.push_back(node_veer_ptr);
                            }
                        });
                        auto slots_count = node_veer_list.size();
                        if (slots_count == 1) // Swap panes in split.
                        {
                            node_veer_list.front()->back()->base::riseup(tier::release, app::tile::events::ui::swap, gear);
                        }
                        else if (slots_count)// Swap selected panes cyclically.
                        {
                            auto emp_slot = sptr{};
                            auto app_slot = sptr{};
                            auto emp_next = sptr{};
                            auto app_next = sptr{};
                            for (auto& s : node_veer_list)
                            {
                                if (s->count() == 1) // empty only
                                {
                                    app_next.reset();
                                    pro::focus::cut(s->back());
                                    emp_next = s->pop_back();
                                }
                                else if (s->count() == 2) // empty + app
                                {
                                    if (auto app = s->back())
                                    {
                                        app->base::signal(tier::release, tile::events::delist, true);
                                    }
                                    pro::focus::cut(s->back());
                                    app_next = s->pop_back();
                                    pro::focus::cut(s->back());
                                    emp_next = s->pop_back();
                                }
                                if (emp_slot)
                                {
                                    if (focus_history_ptr) focus_history_ptr->bind(emp_slot, s);
                                    s->attach(emp_slot);
                                    if (!app_slot) pro::focus::set(emp_slot, gear.id, solo::off); // Refocus.
                                }
                                if (app_slot)
                                {
                                    if (focus_history_ptr) focus_history_ptr->bind(app_slot, s);
                                    s->attach(app_slot);
                                    pro::focus::set(app_slot, gear.id, solo::off); // Refocus.
                                    app_slot->base::riseup(tier::release, tile::events::enlist, app_slot);
                                }
                                std::swap(emp_slot, emp_next);
                                std::swap(app_slot, app_next);
                            }
                            auto& first_item_ptr = node_veer_list.front();
                            if (emp_slot)
                            {
                                if (focus_history_ptr) focus_history_ptr->bind(emp_slot, first_item_ptr);
                                first_item_ptr->attach(emp_slot);
                                if (!app_slot) pro::focus::set(emp_slot, gear.id, solo::off); // Refocus.
                            }
                            if (app_slot)
                            {
                                if (focus_history_ptr) focus_history_ptr->bind(app_slot, first_item_ptr);
                                first_item_ptr->attach(app_slot);
                                pro::focus::set(app_slot, gear.id, solo::off); // Refocus.
                                app_slot->base::riseup(tier::release, tile::events::enlist, app_slot);
                            }
                        }
                        for (auto& s : node_grip_list) // Swap panes in split.
                        {
                            s->back()->base::riseup(tier::release, app::tile::events::ui::swap, gear);
                        }
                        boss.base::broadcast(tier::anycast, e2::form::upon::started);
                        gear.set_handled();
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::create, gear)
                    {
                        foreach(gear.id, [&](auto& item_ptr, si32 item_type, auto)
                        {
                            if (item_type == item_type::empty_slot)
                            {
                                item_ptr->base::riseup(tier::request, e2::form::proceed::createby, gear);
                                gear.set_handled();
                            }
                        });
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::selectapp, request)
                    {
                        auto& gear = request.gear;
                        auto data = get_apps_data(boss);
                        if (!data.ids.empty())
                        {
                            auto index = data.selected_index;
                            if (index == std::numeric_limits<size_t>::max()) index = 0;
                            else
                            {
                                auto count = (si32)data.ids.size();
                                auto dir = request.dir;
                                index = (index + count + (dir % count)) % count;
                            }
                            auto new_selected_id = data.ids[index];
                            auto new_label = data.labels[index];
                            boss.base::property("tile.selected") = new_selected_id;
                            root_veer().base::property("tile.selected") = new_selected_id;
                            boss.base::broadcast(tier::release, e2::form::prop::any, new_label);
                        }
                        gear.set_handled();
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::setapp, new_id)
                    {
                        auto data = get_apps_data(boss);
                        auto found = false;
                        auto new_label = new_id;
                        for (auto i = 0u; i < data.ids.size(); i++)
                        {
                            if (data.ids[i] == new_id)
                            {
                                new_label = data.labels[i];
                                found = true;
                                break;
                            }
                        }
                        if (!found && data.ids.empty()) return;
                        boss.base::property("tile.selected") = new_id;
                        root_veer().base::property("tile.selected") = new_id;
                        boss.base::broadcast(tier::release, e2::form::prop::any, new_label);
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::selected_app, state_ptr)
                    {
                        if (state_ptr)
                        {
                            auto data = get_apps_data(boss);
                            state_ptr->label = data.selected_label.empty() ? data.selected_id : data.selected_label;
                            state_ptr->id = data.selected_id;
                        }
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::select, gear)
                    {
                        foreach(id_t{}, [&](auto& item_ptr, si32 item_type, auto)
                        {
                            if (item_type != item_type::grip) pro::focus::set(item_ptr, gear.id, solo::off);
                            else                              pro::focus::off(item_ptr, gear.id);
                        });
                        gear.set_handled();
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::split::any, gear)
                    {
                        if (root_veer().count() > 2)
                        {
                            root_veer().base::riseup(tier::release, e2::form::proceed::attach);
                            if (root_veer().count() > 2)
                            {
                                gear.set_handled();
                                return;
                            }
                        }
                        auto deed = boss.bell::protos();
                        foreach(gear.id, [&](auto& item_ptr, si32 /*item_type*/, auto node_veer_ptr)
                        {
                            auto room = node_veer_ptr->base::size() / 3;
                            if (room.x && room.y) // Suppress split if there is no space.
                            {
                                boss.base::enqueue([&, deed, gear_id = gear.id, item_wptr = ptr::shadow(item_ptr)](auto& /*boss*/) // Enqueue to keep the focus tree intact while processing events.
                                {
                                    if (auto gear_ptr = boss.base::template getref<hids>(gear_id))
                                    if (auto item_ptr = item_wptr.lock())
                                    {
                                        item_ptr->base::raw_riseup(tier::release, deed, *gear_ptr);
                                    }
                                });
                            }
                            gear.set_handled();
                        });
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::rotate, gear)
                    {
                        foreach(gear.id, [&](auto& item_ptr, si32 /*item_type*/, auto)
                        {
                            item_ptr->base::riseup(tier::release, app::tile::events::ui::rotate, gear);
                            gear.set_handled();
                        });
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::equalize, gear)
                    {
                        foreach(gear.id, [&](auto& item_ptr, si32 /*item_type*/, auto)
                        {
                            item_ptr->base::riseup(tier::release, app::tile::events::ui::equalize, gear);
                            gear.set_handled();
                        });
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::title , gear)
                    {
                        app::shared::set_title(boss, gear);
                        gear.set_handled();
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::zoom, gear)
                    {
                        if (root_veer().count() > 2)
                        {
                            root_veer().base::riseup(tier::release, e2::form::proceed::attach);
                            gear.set_handled();
                        }
                        else
                        {
                            foreach(gear.id, [&](auto& item_ptr, si32 item_type, auto)
                            {
                                if (item_type != item_type::grip)
                                {
                                    item_ptr->base::riseup(tier::preview, e2::form::size::enlarge::maximize, gear);
                                    gear.set_handled();
                                    item_ptr.reset(); // Stop iteration.
                                }
                            });
                        }
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::close, gear)
                    {
                        foreach(gear.id, [&](auto& item_ptr, si32 item_type, auto)
                        {
                            if (item_type != item_type::grip)
                            {
                                item_ptr->base::riseup(tier::preview, e2::form::proceed::quit::one, true);
                                gear.set_handled();
                            }
                        });
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::closeslot, gear)
                    {
                        foreach(gear.id, [&](auto& item_ptr, si32 item_type, auto node_veer_ptr)
                        {
                            if (item_type == item_type::grip) return;
                            if (item_type != item_type::applet) // Empty slot: reuse close-pane path.
                            {
                                item_ptr->base::riseup(tier::preview, e2::form::proceed::quit::one, true);
                                gear.set_handled();
                                return;
                            }
                            // Applet: close pane then remove slot (FIFO enqueue guarantees order).
                            auto veer_wptr = ptr::shadow(node_veer_ptr);
                            item_ptr->base::riseup(tier::preview, e2::form::proceed::quit::one, true);
                            boss.base::enqueue([veer_wptr](auto& /*boss*/)
                            {
                                if (auto veer_ptr = veer_wptr.lock())
                                if (veer_ptr->count() == 1) // Pane closed.
                                {
                                    veer_ptr->base::signal(tier::release, e2::form::proceed::quit::one, true);
                                }
                            });
                            gear.set_handled();
                        });
                    };
                    boss.LISTEN(tier::preview, app::tile::events::ui::rerun, gear)
                    {
                        foreach(gear.id, [&](auto& item_ptr, si32 item_type, auto node_veer_ptr)
                        {
                            if (item_type == item_type::grip) return;
                            if (item_type == item_type::empty_slot)
                            {
                                // Fast path: slot is already empty, run immediately.
                                node_veer_ptr->base::signal(tier::request, e2::form::proceed::createby, gear);
                                gear.set_handled();
                                return;
                            }
                            // Slot has an applet: close it then spawn a new instance.
                            // The veer's tier::release quit::any handler calls bell::expire(),
                            // which terminates the dispatch chain — so a co-listener on the
                            // same event would never run. Instead we rely on the close path's
                            // documented async behaviour: tier::preview quit::one on the veer
                            // (line ~1819) enqueues the actual quit::one+pop_back as a task on
                            // the boss's queue. Enqueueing the createby AFTER that riseup
                            // therefore guarantees FIFO ordering: pop_back runs first, our
                            // task runs next when the slot is empty.
                            auto gear_id = gear.id;
                            auto veer_wptr = ptr::shadow(node_veer_ptr);
                            item_ptr->base::riseup(tier::preview, e2::form::proceed::quit::one, true);
                            boss.base::enqueue([gear_id, veer_wptr](auto& boss)
                            {
                                if (auto veer_ptr = veer_wptr.lock())
                                if (veer_ptr->count() == 1) // Slot must be empty (pop_back done).
                                if (auto gear_ptr = boss.base::template getref<hids>(gear_id))
                                {
                                    veer_ptr->base::signal(tier::request, e2::form::proceed::createby, *gear_ptr);
                                }
                            });
                            gear.set_handled();
                        });
                    };
                });
            // Forward release: upon::started from wrapper (cake) to the object
            // (fork registered as basename::tile), so that script handlers
            // subscribed via source="tile" (e.g., SelectedApplication label)
            // receive the event. builder() fires release: upon::started on the
            // applet (wrapper), but scripts subscribe on the tile object (fork).
            {
                auto object_ptr = object->This();
                wrapper->invoke([object_ptr](auto& boss)
                {
                    boss.LISTEN(tier::release, e2::form::upon::started, root_ptr, -, (object_ptr))
                    {
                        object_ptr->base::signal(tier::release, e2::form::upon::started, root_ptr);
                    };
                });
            }
            return wrapper;
        };
    }

    /*
     * tile: Session manager for standalone tile mode.
     * Manages a single tile applet shared across all clients.
     *
     * Threading model (similar to desktop mode):
     * - Main thread: Accepts connections via server->meet(), submits tasks to async pool
     * - Async pool: Client connections run in parallel via async.run()
     */
    struct hall
    {
        // Thread pool for parallel client connection handling
        netxs::generics::pool async;

        // Shared tile applet instance. All clients connect to this same applet.
        // Terminals are detached when clients disconnect, preserving state.
        sptr applet;

        hall(xipc server, eccc appcfg)
        {
            auto& canal = *server;
            auto& indexer = ui::tui_domain();
            auto& config = indexer.config;
            auto ui_lock = indexer.unique_lock();
            app::shared::get_tui_config(config, ui::skin::globals());
            applet = app::shared::builder(app::tile::id)(appcfg, config);
            app::shared::applet_kb_navigation(config, applet);
            ui_lock.unlock();

            applet->LISTEN(tier::general, e2::shutdown, msg)
            {
                canal.stop();
            };
        }

        // Submit a client handler task to the async pool
        template<class P>
        void submit(P process)
        {
            async.run(process);
        }

        // Handle a single client connection (called from async pool)
        auto invite(xipc client, auto& packet, si32 session_id)
        {
            auto& indexer = ui::tui_domain();
            auto lock = indexer.unique_lock();
            auto gate = ui::gate::ctor(client, packet.mode, packet.user, session_id);
            gate->preserve_on_close = true;
            gate->base::resize(packet.win);
            applet->base::resize(packet.win);
            gate->attach(applet);
            gate->base::signal(tier::release, ui::e2::form::proceed::multihome, applet);
            gate->base::reflow();
            gate->base::deface();
            gate->base::signal(tier::general, e2::config::fps, ui::skin::globals().maxfps);
            ui::pro::focus::set(applet, id_t{}, 1);

            lock.unlock();
            gate->launch(lock);  // Blocks until disconnect
            lock.lock();

            // Detach terminals from gate so they persist after client disconnect
            if (gate->base::subset.size())
            {
                applet->base::holder = gate->base::subset.begin();
                applet->base::father = gate->This();
                applet->base::detach();
            }
        }

        // Main server loop. Runs in the main thread.
        // Accepts client connections and submits them to the async pool.
        auto run(auto& server, auto& userid, auto& prefix)
        {
            log("%%Tile session started"
                "\n      user: %userid%"
                "\n      pipe: %prefix%", prompt::main, userid.first, prefix);

            while (auto user = server->meet())
            {
                if (user->auth(userid.second))
                {
                    auto userinit = directvt::binary::init{};
                    if (auto packet = userinit.recv(user))
                    {
                        // Submit client handler to async pool
                        // Note: async.run() passes thread ID as int parameter
                        submit([&, user, packet](auto session_id)
                        {
                            auto client_id = utf::concat(*user);
                            if constexpr (debugmode) log("%%Client connected %id%", prompt::user, client_id);

                            invite(user, packet, session_id);

                            if constexpr (debugmode) log("%%Client disconnected %id%", prompt::user, client_id);
                        });
                    }
                }
            }

            return 0;
        }

        // Shutdown: Wait for all async tasks to complete
        void stop()
        {
            log("%%Tile shutdown", prompt::main);
            applet->base::signal(tier::general, e2::conio::quit); // Trigger to disconnect all users.
            async.stop();  // Wait for all client tasks to complete
            if constexpr (debugmode) log("%%Async pool stopped", prompt::main);
        }
    };

    app::shared::initialize builder{ app::tile::id, build_inst };
}
