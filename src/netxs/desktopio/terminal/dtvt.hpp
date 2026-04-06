    struct dtvt
        : public ui::form<dtvt>
    {
        static constexpr auto classname = basename::dtvt;

        std::unordered_map<id_t, netxs::sptr<input::tooltip_t>> tooltips;
        auto& get_tooltip_reference(id_t gear_id)
        {
            auto iter = tooltips.find(gear_id);
            if (iter == tooltips.end())
            {
                iter = tooltips.emplace(gear_id, ptr::shared<input::tooltip_t>()).first;
            }
            auto& tooltip_sptr = iter->second;
            return tooltip_sptr;
        }

        // dtvt: Event handler.
        struct link : s11n, input_fields_handler
        {
            using input_fields_handler::handle;

            dtvt& owner; // link: Terminal object reference.

            void handle(s11n::xs::bitmap_dtvt       /*lock*/)
            {
                owner.base::enqueue([&](auto& /*boss*/) mutable
                {
                    owner.base::deface();
                });
            }
            void handle(s11n::xs::jgc_list            lock)
            {
                s11n::receive_jgc(lock);
                owner.base::enqueue([&](auto& /*boss*/) mutable
                {
                    owner.base::deface();
                });
            }
            void handle(s11n::xs::tooltips            lock)
            {
                owner.base::enqueue([&, tooltips = lock.thing](auto& /*boss*/) mutable
                {
                    for (auto& tooltip : tooltips)
                    {
                        auto& tooltip_sptr = owner.get_tooltip_reference(tooltip.gear_id);
                        tooltip_sptr->set(tooltip.utf8);
                    }
                });
            }
            void handle(s11n::xs::fullscrn            lock)
            {
                auto m = lock.thing;
                lock.unlock();
                if (owner.active)
                {
                    auto guard = owner.sync();
                    if (auto gear_ptr = owner.base::getref<hids>(m.gear_id))
                    if (auto parent_ptr = owner.base::parent())
                    {
                        auto& gear = *gear_ptr;
                        gear.set_multihome();
                        if (gear.captured(owner.id)) gear.setfree();
                        parent_ptr->base::riseup(tier::preview, e2::form::size::enlarge::fullscreen, gear);
                    }
                }
            }
            void handle(s11n::xs::maximize            lock)
            {
                auto m = lock.thing;
                lock.unlock();
                if (owner.active)
                {
                    auto guard = owner.sync();
                    if (auto gear_ptr = owner.base::getref<hids>(m.gear_id))
                    if (auto parent_ptr = owner.base::parent())
                    {
                        auto& gear = *gear_ptr;
                        gear.set_multihome();
                        if (gear.captured(owner.id)) gear.setfree();
                        parent_ptr->base::riseup(tier::preview, e2::form::size::enlarge::maximize, gear);
                    }
                }
            }
            void handle(s11n::xs::sysfocus            lock)
            {
                auto f = lock.thing;
                lock.unlock();
                if (owner.active)
                {
                    auto guard = owner.sync(); // Guard the owner.This() call.
                    auto owner_ptr = owner.This();
                    if (f.state)
                    {
                        owner.base::signal(tier::request, input::events::focus::add, { .gear_id = f.gear_id, .focus_type = f.focus_type });
                    }
                    else
                    {
                        owner.base::signal(tier::request, input::events::focus::rem, { .gear_id = f.gear_id });
                    }
                }
            }
            void handle(s11n::xs::syskeybd            lock)
            {
                auto k = lock.thing;
                lock.unlock();
                if (owner.active)
                {
                    auto guard = owner.sync();
                    if (auto gear_ptr = owner.base::getref<hids>(k.gear_id))
                    {
                        auto& gear = *gear_ptr;
                        gear.set_multihome();
                        gear.keybd::vkevent = owner.indexer.get_kbchord_hint(k.vkchord);
                        gear.keybd::scevent = owner.indexer.get_kbchord_hint(k.scchord);
                        gear.keybd::chevent = owner.indexer.get_kbchord_hint(k.chchord);
                        k.syncto(gear);
                        //owner.base::riseup(tier::release, input::events::keybd::post, gear, true);
                        pro::keybd::forward_release(owner, gear);
                    }
                }
            };
            void handle(s11n::xs::mouse_event         lock)
            {
                auto m = lock.thing;
                lock.unlock();
                if (owner.active)
                {
                    auto guard = owner.sync();
                    if (auto gear_ptr = owner.base::getref<hids>(m.gear_id))
                    if (auto parent_ptr = owner.base::parent())
                    {
                        auto& gear = *gear_ptr;
                        auto& parent = *parent_ptr;
                        gear.set_multihome();
                        if (gear.captured(owner.id)) gear.setfree();
                        auto basis = gear.owner.base::coor();
                        owner.global(basis);
                        gear.replay(parent, m.cause, m.coord - basis, m.click - basis, m.delta, m.buttons, m.bttn_id, m.dragged, m.ctlstat, m.whlfp, m.whlsi, m.hzwhl);
                    }
                }
            }
            void handle(s11n::xs::minimize            lock)
            {
                owner.base::enqueue([&, m = lock.thing](auto& /*boss*/)
                {
                    if (auto gear_ptr = owner.base::getref<hids>(m.gear_id))
                    {
                        auto& gear = *gear_ptr;
                        gear.set_multihome();
                        owner.base::riseup(tier::preview, e2::form::size::minimize, gear);
                    }
                });
            }
            void handle(s11n::xs::expose            /*lock*/)
            {
                owner.base::enqueue([&](auto& /*boss*/)
                {
                    owner.base::riseup(tier::preview, e2::form::layout::expose);
                });
            }
            void handle(s11n::xs::clipdata            lock)
            {
                auto c = lock.thing;
                lock.unlock();
                if (owner.active)
                {
                    auto guard = owner.sync();
                    if (auto gear_ptr = owner.base::getref<hids>(c.gear_id))
                    {
                        gear_ptr->set_multihome();
                        gear_ptr->set_clipboard(c);
                    }
                }
            }
            void handle(s11n::xs::clipdata_request    lock)
            {
                auto c = lock.thing;
                lock.unlock();
                if (owner.active)
                {
                    auto guard = owner.sync();
                    if (auto gear_ptr = owner.base::getref<hids>(c.gear_id))
                    {
                        auto& gear = *gear_ptr;
                        gear.set_multihome();
                        gear.owner.base::signal(tier::request, input::events::clipboard, gear);
                        auto& data = gear.board::cargo;
                        if (data.hash != c.hash)
                        {
                            s11n::clipdata.send(owner, c.gear_id, data.hash, data.size, data.utf8, data.form, data.meta);
                            return;
                        }
                    }
                    else log(prompt::dtvt, ansi::err("Unregistered input device id: ", c.gear_id));
                    s11n::clipdata.send(owner, c.gear_id, c.hash, dot_00, text{}, mime::ansitext, text{});
                }
            }
            void handle(s11n::xs::header              lock)
            {
                owner.base::enqueue([&, /*id = h.window_id,*/ header = lock.thing.utf8](auto& /*boss*/) mutable
                {
                    owner.base::riseup(tier::preview, e2::form::prop::ui::header, header);
                });
            }
            void handle(s11n::xs::footer              lock)
            {
                owner.base::enqueue([&, /*id = f.window_id,*/ footer = lock.thing.utf8](auto& /*boss*/) mutable
                {
                    owner.base::riseup(tier::preview, e2::form::prop::ui::footer, footer);
                });
            }
            void handle(s11n::xs::header_request      lock)
            {
                auto c = lock.thing;
                lock.unlock();
                if (owner.active)
                {
                    //todo use window_id
                    auto guard = owner.sync();
                    auto header_utf8 = owner.base::riseup(tier::request, e2::form::prop::ui::header);
                    s11n::header.send(owner, c.window_id, header_utf8);
                }
            }
            void handle(s11n::xs::footer_request      lock)
            {
                auto c = lock.thing;
                lock.unlock();
                if (owner.active)
                {
                    //todo use window_id
                    auto guard = owner.sync();
                    auto footer_utf8 = owner.base::riseup(tier::request, e2::form::prop::ui::footer);
                    s11n::footer.send(owner, c.window_id, footer_utf8);
                }
            }
            void handle(s11n::xs::warping             lock)
            {
                owner.base::enqueue([&, /*id = w.window_id,*/ warp = lock.thing.warpdata](auto& /*boss*/)
                {
                    //todo use window_id
                    owner.base::riseup(tier::preview, e2::form::layout::swarp, warp);
                });
            }
            void handle(s11n::xs::logs                lock)
            {
                s11n::recycle_log(lock, os::process::id.second);
            }
            void handle(s11n::xs::sysclose            lock)
            {
                lock.unlock();
                auto guard = owner.sync();
                owner.active.exchange(faux);
                owner.stop(true);
            }
            void handle(s11n::xs::sysstart          /*lock*/)
            {
                owner.base::enqueue([&](auto& /*boss*/)
                {
                    owner.base::riseup(tier::release, e2::form::global::sysstart, 1);
                });
            }
            void handle(s11n::xs::cwd                 lock)
            {
                owner.base::enqueue([&, path = lock.thing.path](auto& /*boss*/)
                {
                    owner.base::riseup(tier::preview, e2::form::prop::cwd, path);
                });
            }
            void handle(s11n::xs::gui_command         lock)
            {
                owner.base::enqueue([&, gui_cmd = lock.thing](auto& /*boss*/)
                {
                    if (auto gear_ptr = owner.base::getref<hids>(gui_cmd.gear_id))
                    {
                        gear_ptr->set_multihome();
                        owner.base::riseup(tier::preview, e2::command::gui, gui_cmd);
                    }
                });
            }

            link(dtvt& owner)
                : s11n{ *this, owner.id },
                  input_fields_handler{ owner },
                  owner{ owner }
            { }
        };

        struct msgs
        {
            static constexpr auto no_signal = "NO SIGNAL"sv;
        };

        using vtty = os::dtvt::vtty;

        link stream; // dtvt: Event handler.
        flag active; // dtvt: Terminal lifetime.
        si32 opaque; // dtvt: Object transparency on d_n_d (no pro::cache).
        si32 nodata; // dtvt: Show splash "No signal".
        face splash; // dtvt: "No signal" splash.
        page errmsg; // dtvt: Overlay error message.
        vtty ipccon; // dtvt: IPC connector. Should be destroyed first.

        // dtvt: Format error message overlay.
        auto genmsg(view utf8) -> page
        {
            auto data = para{ utf8 };
            auto kant = text(2, ' ');
            auto pads = text(data.length() + kant.size() * 2, ' ');
            return page{ ansi::bgc(reddk).fgc(whitelt).jet(bias::center).wrp(wrap::off).cup(dot_00).cpp({50,50}).cuu(1)
                              .add(pads,       '\n',
                             kant, utf8, kant, '\n',
                                   pads,       '\n') };
        }
        // dtvt: Send client data.
        void output(view data)
        {
            ipccon.output(data);
        }
        void sync_window_state()
        {
            if (ipccon)
            {
                auto state = base::riseup(tier::request, e2::form::prop::window::state);
                stream.window_state.send(*this, state);
            }
        }
        // dtvt: Attach a new process.
        template<class T = noop>
        void start_dtvt(eccc& appcfg, T connect_fx = {})
        {
            if (ipccon)
            {
                active.exchange(faux); // Do not show "Disconnected".
                ipccon.payoff();
            }
            errmsg = genmsg(msgs::no_signal);
            nodata = {};
            stream.syswinsz.freeze().thing.winsize = {};
            active.exchange(true);
            auto receiver_fx = [&](view utf8)
            {
                if (active)
                {
                    stream.sync(utf8);
                    stream.request_jgc(*this);
                }
            };
            auto shutdown_fx = [&]()
            {
                base::enqueue([&](auto& /*boss*/)
                {
                    ipccon.payoff(); // Join input thread.
                    if constexpr (std::is_same_v<decltype(connect_fx), noop>) // ui::term manages the child process's lifetime.
                    {
                        base::signal(tier::anycast, e2::form::proceed::quit::one, true);
                    }
                });
            };
            ipccon.run_dtvt_app(appcfg, base::size(), connect_fx, receiver_fx, shutdown_fx);
            sync_window_state();
        }
        // dtvt: Return true if application has never sent its canvas.
        auto is_nodtvt()
        {
            auto lock = stream.bitmap_dtvt.freeze();
            auto& canvas = lock.thing.image;
            return !canvas.hash(); // Canvas never resized/received.
        }
        // dtvt: Close dtvt-object.
        void stop(bool fast, bool notify = true)
        {
            if (notify)
            {
                base::signal(tier::request, e2::form::proceed::quit::one, fast);
            }
            if (is_nodtvt()) // Terminate a non-dtvt-aware application that has never sent its canvas.
            {
                ipccon.abort();
            }
            else if (fast && active.exchange(faux) && ipccon) // Notify and queue closing immediately.
            {
                stream.sysclose.send(*this, fast);
            }
            else if (active && ipccon) // Just notify if active. Queue closing if not.
            {
                stream.sysclose.send(*this, fast);
                return;
            }
            base::enqueue<faux>([&, backup = This()](auto& /*boss*/) mutable
            {
                ipccon.payoff();
                auto lock = bell::sync();
                base::riseup(tier::release, e2::form::proceed::quit::one, true);
                backup.reset(); // Backup should dtored under the lock.
            });
        }
        // dtvt: Splash screen if there is no next frame.
        void fallback(core const& canvas, bool forced = faux, bool show_msg = true)
        {
            auto size = base::size();
            if (splash.size() != size || forced)
            {
                splash.size(size);
                auto parent_id = id_t{}; // Handover control to the parent if no response.
                if (auto parent = base::parent()) parent_id = parent->id;
                if (canvas.size())
                {
                    if (show_msg)
                    {
                        auto tmpbuf = vrgb{};
                        splash.zoom(canvas, cell::shaders::onlyid(parent_id));
                        splash.output(errmsg);
                        splash.blur(2, tmpbuf, [](cell& c){ c.fgc(argb::transit(c.bgc(), c.fgc(), 127)); });
                        splash.output(errmsg);
                    }
                    else
                    {
                        splash.fill(canvas, cell::shaders::onlyid(parent_id));
                    }
                }
                // splash.wipe(cell{}.link(parent_id).fgc(blacklt).bgc(blacklt).alpha(0x40)); // Temporarily disabled to test gray background issue
            }
        }
        // dtvt: Render next frame.
        void fill(core& parent_canvas, core const& canvas)
        {
            if (opaque == 0xFF) parent_canvas.fill(canvas, cell::shaders::overlay);
            else                parent_canvas.fill(canvas, cell::shaders::transparent(opaque));
        }

        dtvt()
            : stream{*this },
              active{ true },
              opaque{ 0xFF },
              nodata{      }
        {
            LISTEN(tier::release, input::events::device::mouse::any, gear)
            {
                if (gear.captured(base::id))
                {
                    if (!gear.m_sys.buttons) gear.setfree();
                }
                else if (gear.m_sys.buttons) gear.capture(base::id);
                gear.m_sys.gear_id = gear.id;
                stream.sysmouse.send(*this, gear.m_sys);
                gear.dismiss();
            };
            LISTEN(tier::general, input::events::die, gear)
            {
                gear.setfree();
                gear.m_sys.gear_id = gear.id;
                gear.m_sys.enabled = hids::stat::die;
                stream.sysmouse.send(*this, gear.m_sys);
            };
            on(tier::mouserelease, input::key::MouseHover, [&](hids& gear)
            {
                auto& tooltip_sptr = get_tooltip_reference(gear.id);
                gear.tooltip.set(tooltip_sptr); // Set tooltip reference.
            });
            on(tier::mouserelease, input::key::MouseLeave, [&](hids& gear)
            {
                gear.m_sys.gear_id = gear.id;
                gear.m_sys.enabled = hids::stat::halt;
                stream.sysmouse.send(*this, gear.m_sys);
            });
            bell::dup_handler(tier::general, input::events::halt.id);
            LISTEN(tier::release, input::events::focus::set::any, seed)
            {
                auto deed = bell::protos();
                auto state = deed == input::events::focus::set::on.id;
                stream.sysfocus.send(*this, seed.gear_id, state, seed.focus_type, seed.treeid, seed.digest);
            };
            LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                gear.gear_id = gear.id;
                stream.syskeybd.send(*this, gear);
                gear.dismiss();
            };
            LISTEN(tier::anycast, e2::form::prop::cwd, path)
            {
                stream.cwd.send(*this, path);
            };
            LISTEN(tier::release, e2::form::prop::window::state, state)
            {
                sync_window_state();
            };
            LISTEN(tier::release, e2::area, new_area)
            {
                //todo implement deform/inform (for incoming XTWINOPS/swarp)
                auto winsize = stream.syswinsz.freeze().thing.winsize;
                if (ipccon && winsize != new_area.size)
                {
                    stream.syswinsz.send(*this, 0, new_area.size, faux);
                }
            };
            LISTEN(tier::anycast, e2::form::prop::lucidity, value)
            {
                if (value == -1) value = opaque;
                else             opaque = value;
            };
            auto& maxoff = base::field(span{ span::period::den / std::max(1, ui::skin::globals().maxfps) }); // dtvt: Max delay before showing "No signal".
            LISTEN(tier::general, e2::config::fps, fps)
            {
                maxoff = span{ span::period::den / std::max(1, fps) };
                if (fps > 0)
                {
                    stream.fps.send(*this, fps);
                }
            };
            LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                auto size = base::size();
                auto lock = stream.bitmap_dtvt.freeze();
                auto& canvas = lock.thing.image;
                if (nodata == canvas.hash()) // " No signal " on timeout > 1/60s
                {
                    fallback(canvas);
                    fill(parent_canvas, splash);
                    return;
                }
                else if (size == canvas.size())
                {
                    fill(parent_canvas, canvas);
                }
                else if (canvas.size())
                {
                    while (size != canvas.size()) // Always waiting for the correct frame.
                    {
                        if (!active) return;
                        if (std::cv_status::timeout == lock.wait_for(maxoff)
                         && size != canvas.size())
                        {
                            nodata = canvas.hash();
                            fallback(canvas, faux, faux);
                            fill(parent_canvas, splash);
                            return;
                        }
                    }
                    fill(parent_canvas, canvas);
                }
            };
        }
    };
