        // term: VT-style mouse tracking functionality.
        struct m_tracking
        {
            using mode = input::mouse::mode;
            using prot = input::mouse::prot;

            term& owner; // m_tracking: Terminal object reference.
            fp2d  coord; // m_tracking: Last coord of mouse cursor.
            subs  token; // m_tracking: Subscription token.
            prot  encod; // m_tracking: Mouse encoding protocol.
            mode  state; // m_tracking: Mouse reporting mode.
            si32  smode; // m_tracking: Selection mode state backup.

            m_tracking(term& owner)
                : owner{ owner                   },
                  encod{ prot::x11               },
                  state{ mode::none              },
                  smode{ owner.defcfg.def_selmod }
            { }

            operator bool () { return state != mode::none; }

            void check_focus(hids& gear) // Set keybd focus on any click if it is not set.
            {
                auto m = std::bitset<8>{ (ui32)gear.m_sys.buttons };
                auto s = std::bitset<8>{ (ui32)gear.m_sav.buttons };
                if (m[hids::buttons::right] && !s[hids::buttons::right])
                {
                    auto gear_test = owner.base::riseup(tier::request, e2::form::state::keybd::find, { gear.id, 0 });
                    if (gear_test.second == 0)
                    {
                        pro::focus::set(owner.This(), gear.id, solo::off);
                    }
                    owner.base::riseup(tier::preview, e2::form::layout::expose);
                }
                else if ((m[hids::buttons::left  ] && !s[hids::buttons::left  ])
                      || (m[hids::buttons::middle] && !s[hids::buttons::middle]))
                {
                    auto gear_test = owner.base::riseup(tier::request, e2::form::state::keybd::find, { gear.id, 0 });
                    if (gear_test.second == 0)
                    {
                        if (pro::focus::test(owner, gear)) pro::focus::off(owner.This(), gear.id);
                        else                               pro::focus::set(owner.This(), gear.id, gear.meta(hids::anyCtrl) ? solo::off : solo::on);
                    }
                    owner.base::riseup(tier::preview, e2::form::layout::expose);
                }
            }
            void enable(mode m)
            {
                state = (mode)(state | m);
                if (state && !token.size()) // Do not subscribe if it is already subscribed.
                {
                    owner.on(tier::mouserelease, input::key::MouseLeave, token, [&](hids& gear)
                    {
                        if (owner.selmod == mime::disabled)
                        {
                            coord = { fp32nan, fp32nan }; // Forward a mouse halt event.
                            owner.ipccon.mouse(gear, true, coord, encod, state);
                        }
                    });
                    owner.bell::dup_handler(tier::general, input::events::halt.id, token.back());
                    owner.LISTEN(tier::release, input::events::device::mouse::any, gear, token)
                    {
                        check_focus(gear);
                        if (owner.selmod == mime::disabled)
                        {
                            if (gear.captured(owner.id))
                            {
                                if (!gear.m_sys.buttons) gear.setfree();
                            }
                            else if (gear.m_sys.buttons) gear.capture(owner.id);
                            auto& console = *owner.target;
                            auto c = gear.m_sys.coordxy;
                            c.y -= console.get_basis();
                            auto moved = coord((state & mode::over) ? c
                                                                    : std::clamp(c, fp2d{ dot_00 }, fp2d{ console.panel - dot_11 }));
                            if (gear.m_sav.changed != gear.m_sys.changed)
                            {
                                owner.ipccon.mouse(gear, moved, coord, encod, state);
                            }
                            gear.dismiss();
                        }
                    };
                    smode = owner.selmod;
                }
                owner.selection_selmod(mime::disabled);
            }
            void disable(mode m)
            {
                state = (mode)(state & ~(m));
                if (!state) token.clear();
                owner.selection_selmod(smode);
                auto gates = owner.base::riseup(tier::request, e2::form::state::keybd::enlist); // Take all foci.
                for (auto gate_id : gates) // Reset double click state for all gears.
                {
                    if (auto gear_ptr = owner.base::getref<hids>(gate_id))
                    {
                        for (auto& [bttn_id, s] : gear_ptr->stamp) // Reset double click state. The issue is related to Far Manager, which changes the mouse tracking mode before releasing the button when double-clicking.
                        {
                            s.count = !!s.count; // Set to 1 if non zero.
                        }
                    }
                }
            }
            void reset()
            {
                if (state != mode::none) disable(state);
                encod = prot::x11;
                coord = {};
                smode = owner.selmod;
            }
            void setmode(prot p) { encod = p; }
        };
