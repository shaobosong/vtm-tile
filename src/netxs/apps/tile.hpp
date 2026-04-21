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
                EVENT_XS( selected_app, app_state* ), // Get selected app info.
                EVENT_XS( close   , input::hids ), // Close panes.
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
            if (!item_id.empty())
            {
                if (item_id == selected_id)
                {
                    res.selected_index = res.ids.size();
                    res.selected_label = item_label;
                }
                res.ids.push_back(item_id);
                res.labels.push_back(item_label);
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
        X(SelectApplication  ) \
        X(SelectedApp        ) \
        X(SelectAllPanes     ) \
        X(SplitPane          ) \
        X(RotateSplit        ) \
        X(SwapPanes          ) \
        X(EqualizeSplitRatio ) \
        X(SetTitle           ) \
        X(ZoomPane           ) \
        X(ClosePane          ) \
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
        X(OpenWorkspacePopup ) \

    struct methods
    {
        #define X(_proc) static constexpr auto _proc = #_proc;
        proc_list
        #undef X
    };

    #undef proc_list

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
                                item_ptr->base::signal(tier::release, e2::form::proceed::quit::one, fast);
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
                            pro::focus::set(app, gear.id, solo::on);
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
            auto wrapper = ui::cake::ctor()
                ->plugin<pro::focus>();
            auto object = wrapper->attach(ui::fork::ctor(axis::Y))
                ->plugin<items>()
                ->plugin<pro::focus>()
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

            // Inner fork: slot::_1 = workspace host (flexible), slot::_2 = status bar (fixed 1 row).
            auto inner_fork_ptr = object->attach(slot::_2, ui::fork::ctor(axis::Y, 0, 1, 0));
            auto workspace_host_ptr = inner_fork_ptr->attach(slot::_1, ui::veer::ctor()
                ->plugin<pro::focus>());
            auto status_bar_ptr = inner_fork_ptr->attach(slot::_2, ui::mock::ctor()
                ->limits({ -1, 1 }, { -1, 1 })
                ->active());

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
            auto switch_workspace = [workspaces_ptr, current_ws_index_ptr, previous_ws_index_ptr, workspace_host_ptr, refresh_status_bar_fn](size_t idx) -> bool
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
            auto destroy_workspace = [workspaces_ptr, current_ws_index_ptr, workspace_host_ptr, refresh_status_bar_fn, focus_histories_ptr](size_t idx) -> bool
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

            // Status bar: display only the current workspace index with inactive styling.
            // Click opens the workspace preview popup (Win+Tab style).
            auto hovered_tab = ptr::shared(si32{ -1 }); // Hover state for the single button (-1 = none, 0 = hovered).
            auto ws_popup_active = ptr::shared(faux);    // Whether the workspace preview popup is currently open.
            *refresh_status_bar_fn = [status_bar_ptr]
            {
                status_bar_ptr->base::deface();
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

            status_bar_ptr->invoke([&, workspaces_ptr, current_ws_index_ptr, switch_workspace, create_workspace,
                                      hovered_tab, refresh_status_bar_fn, ws_popup_active,
                                      collect_ws_panes_fn, open_workspace_popup_fn,
                                      wrapper_shadow = ptr::shadow(wrapper)](auto& boss)
            {
                auto boss_id = boss.bell::id;
                // Render: draw only the current workspace index button with inactive styling.
                boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (workspaces_ptr, current_ws_index_ptr, boss_id, hovered_tab))
                {
                    // Clear entire bar.
                    parent_canvas.fill([boss_id](cell& c)
                    {
                        c.bgc(popup_bar_bg).fgc(popup_bar_bg).txt(whitespace).link(boss_id).bld(faux).und(0).ovr(faux);
                    });
                    // Draw current workspace index button with inactive styling.
                    auto hover = *hovered_tab;
                    auto is_hover = (hover == 0);
                    auto bg    = is_hover ? popup_hov_bg : popup_bar_bg;
                    auto fg    = is_hover ? popup_hov_fg : popup_dim_fg;
                    auto uline = is_hover ? unln::dotted : unln::none;
                    auto label = text(1, char(ws_min_index + *current_ws_index_ptr));
                    parent_canvas.fill(rect{{ 0, 0 }, { ws_btn_w, 1 }}, [=](cell& c)
                    {
                        c.bgc(bg).fgc(fg).txt(whitespace).link(boss_id).bld(faux).und(uline).unc(popup_hov_ul);
                    });
                    parent_canvas.fill(rect{{ 1, 0 }, { 1, 1 }}, [=](cell& c)
                    {
                        c.bgc(bg).fgc(fg).txt(label).link(boss_id).bld(faux).und(uline).unc(popup_hov_ul);
                    });
                };
                // Track mouse hover on the single workspace button.
                boss.on(tier::mouserelease, input::key::MouseMove, [hovered_tab, refresh_status_bar_fn](hids& gear)
                {
                    auto x = gear.coord.x;
                    auto new_tab = (x >= 0 && x < ws_btn_w) ? si32{ 0 } : si32{ -1 };
                    if (new_tab != *hovered_tab)
                    {
                        *hovered_tab = new_tab;
                        (*refresh_status_bar_fn)();
                    }
                });
                // Clear hover when mouse leaves the status bar.
                boss.on(tier::mouserelease, input::key::MouseLeave, [hovered_tab, refresh_status_bar_fn](hids& /*gear*/)
                {
                    if (*hovered_tab != -1)
                    {
                        *hovered_tab = -1;
                        (*refresh_status_bar_fn)();
                    }
                });
                // Opens the workspace preview popup (Win+Tab style). Invoked by the status bar click
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

                // Click on the workspace button: open the workspace preview popup.
                boss.on(tier::mouserelease, input::key::LeftClick,
                    [open_workspace_popup_fn](hids& gear)
                {
                    auto x = gear.coord.x;
                    if (x < 0 || x >= ws_btn_w) { gear.dismiss(); return; }
                    (*open_workspace_popup_fn)();
                    gear.dismiss();
                });
            });

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
                        { methods::OpenWorkspacePopup,  [&, open_workspace_popup_fn]
                                                        {
                                                            (*open_workspace_popup_fn)();
                                                        }},
                    });

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
