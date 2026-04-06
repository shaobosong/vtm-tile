        // term: Terminal 16/256 color palette tracking functionality.
        struct c_tracking
        {
            using pals = std::remove_const_t<decltype(argb::vt256)>;
            using func = utf::unordered_map<text, std::function<void(view)>>;

            enum class type { invalid, rgbcolor, request };

            term& owner; // c_tracking: Terminal object reference.
            pals  color; // c_tracking: 16/256 colors palette.
            func  procs; // c_tracking: Handlers.
            escx  reply; // c_tracking: Reply buffer.

            void reset()
            {
                std::copy(std::begin(owner.defcfg.def_colors), std::end(owner.defcfg.def_colors), std::begin(color));
            }
            auto to_byte(char c)
            {
                     if (c >= '0' && c <= '9') return c - '0';
                else if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                else if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                else                           return 0;
            }
            auto record(view& data) -> std::pair<type, ui32> // ; rgb:.../.../...
            {
                utf::trim_front(data, " ;");
                if (data.size() && data.front() == '?') // If a "?" is given rather than a name or RGB specification, termimal should reply with a control sequence of the same form.
                {
                    data.remove_prefix(1);
                    return { type::request, 0 };
                }
                else if (data.starts_with("rgb:"))
                {
                    auto get_color = [&](auto n)
                    {
                        auto r1 = to_byte(data[4]);
                        auto r2 = to_byte(data[5]);
                        auto g1 = to_byte(data[4 + n + 1]);
                        auto g2 = to_byte(data[5 + n + 1]);
                        auto b1 = to_byte(data[4 + (n + 1) * 2]);
                        auto b2 = to_byte(data[5 + (n + 1) * 2]);
                        data.remove_prefix(n * 3 + 4/*rgb:*/ + 2/*//*/);
                        return (b1 << 4 ) + (b2      )
                             + (g1 << 12) + (g2 << 8 )
                             + (r1 << 20) + (r2 << 16)
                             + 0xFF000000;
                    };
                    if (data.length() >= 12 && data[6] == '/' && data[9] == '/') // ; rgb:00/00/00
                    {
                        return { type::rgbcolor, get_color(2) };
                    }
                    else if (data.length() >= 15 && data[7] == '/' && data[11] == '/') // ; rgb:000/000/000
                    {
                        return { type::rgbcolor, get_color(3) };
                    }
                    else if (data.length() >= 18 && data[8] == '/' && data[13] == '/') // ; rgb:0000/0000/0000
                    {
                        return { type::rgbcolor, get_color(4) };
                    }
                }
                else if (data.starts_with("0x")) // ; 0xbbggrr
                {
                    if (data.length() >= 8)
                    {
                        auto b1 = to_byte(data[2]);
                        auto b2 = to_byte(data[3]);
                        auto g1 = to_byte(data[4]);
                        auto g2 = to_byte(data[5]);
                        auto r1 = to_byte(data[6]);
                        auto r2 = to_byte(data[7]);
                        data.remove_prefix(8); // sizeof 0xbbggrr
                        auto c = (b1 << 4 ) + (b2      )
                               + (g1 << 12) + (g2 << 8 )
                               + (r1 << 20) + (r2 << 16)
                               + 0xFF000000;
                        return { type::rgbcolor, c };
                    }
                }
                else if (data.starts_with("#")) // ; #rrggbb
                {
                    if (data.length() >= 7)
                    {
                        auto r1 = to_byte(data[1]);
                        auto r2 = to_byte(data[2]);
                        auto g1 = to_byte(data[3]);
                        auto g2 = to_byte(data[4]);
                        auto b1 = to_byte(data[5]);
                        auto b2 = to_byte(data[6]);
                        data.remove_prefix(7); // sizeof #rrggbb
                        auto c = (b1 << 4 ) + (b2      )
                               + (g1 << 12) + (g2 << 8 )
                               + (r1 << 20) + (r2 << 16)
                               + 0xFF000000;
                        return { type::rgbcolor, c };
                    }
                }
                else // Lookup custom color names stored in settings.xml.
                {
                    auto shadow = data;
                    auto color_name = utf::take_front<faux>(shadow, ";");
                    utf::trim(color_name);
                    auto name_str = utf::name2token(color_name);
                    auto iter = owner.defcfg.color_names.find(name_str);
                    if (iter != owner.defcfg.color_names.end())
                    {
                        data = shadow;
                        return std::pair{ type::rgbcolor, iter->second };
                    }
                }
                return { type::invalid, 0 };
            }
            void notsupported(text const& property, view full_data, view unkn_data)
            {
                log("%%Not supported: OSC=%property%\n\tDATA=%data%\n\tSIZE=%length%\n\tHEX=%hexdata%\n\tUNKN=%%", prompt::term, property, full_data, full_data.length(), utf::buffer_to_hex(full_data), ansi::err(unkn_data));
            }

            c_tracking(term& owner)
                : owner{ owner }
            {
                reset();
                procs[ansi::osc_linux_color] = [&](view data) // ESC ] P Nrrggbb
                {
                    if (data.length() >= 7)
                    {
                        auto n  = to_byte(data[0]);
                        auto r1 = to_byte(data[1]);
                        auto r2 = to_byte(data[2]);
                        auto g1 = to_byte(data[3]);
                        auto g2 = to_byte(data[4]);
                        auto b1 = to_byte(data[5]);
                        auto b2 = to_byte(data[6]);
                        color[n] = (b1 << 4 ) + (b2      )
                                 + (g1 << 12) + (g2 << 8 )
                                 + (r1 << 20) + (r2 << 16)
                                 + 0xFF000000;
                    }
                };
                procs[ansi::osc_reset_color] = [&](view data) // ESC ] 104 ; 0; 1;...
                {
                    auto empty = true;
                    while (data.length())
                    {
                        utf::trim_front_if(data, [](char c){ return c >= '0' && c <= '9'; });
                        if (auto v = utf::to_int(data))
                        {
                            auto n = std::clamp(v.value(), 0, 255);
                            color[n] = argb::vt256[n];
                            empty = faux;
                        }
                    }
                    if (empty) reset();
                };
                procs[ansi::osc_set_palette] = [&](view data) // ESC ] 4 ; 0;rgb:00/00/00;1;rgb:00/00/00;...
                {
                    auto fails = faux;
                    auto full_data = data;
                    while (data.length())
                    {
                        utf::trim_front(data, " ;");
                        if (data.empty()) break;
                        if (auto v = utf::to_int(data))
                        {
                            auto n = std::clamp(v.value(), 0, 255);
                            auto [t, r] = record(data);
                            if (t == type::request)
                            {
                                auto c = argb{ color[n] };
                                reply.osc(ansi::osc_set_palette, utf::fprint("%n%;rgb:%r%/%g%/%b%", n, utf::to_hex(c.chan.r),
                                                                                                       utf::to_hex(c.chan.g),
                                                                                                       utf::to_hex(c.chan.b)));
                            }
                            else if (t == type::rgbcolor)
                            {
                                color[n] = r;
                            }
                            else
                            {
                                fails = true;
                                break;
                            }
                        }
                        else
                        {
                            fails = true;
                            break;
                        }
                    }
                    if (fails) notsupported(ansi::osc_set_palette, full_data, data);
                };
                procs[ansi::osc_linux_reset] = [&](view /*data*/) // ESC ] R
                {
                    reset();
                };
                procs[ansi::osc_set_fgcolor] = [&](view data) // ESC ] 10 ;rgb:00/00/00
                {
                    auto full_data = data;
                    auto [t, r] = record(data);
                    if (t == type::request)
                    {
                        auto c = owner.target->brush.sfg();
                        reply.osc(ansi::osc_set_fgcolor, utf::fprint("rgb:%r%/%g%/%b%", utf::to_hex(c.chan.r),
                                                                                        utf::to_hex(c.chan.g),
                                                                                        utf::to_hex(c.chan.b)));
                    }
                    else if (t == type::rgbcolor)
                    {
                        owner.target->brush.sfg(r);
                    }
                    else notsupported(ansi::osc_set_fgcolor, full_data, data);
                };
                procs[ansi::osc_set_bgcolor] = [&](view data) // ESC ] 11 ;rgb:00/00/00
                {
                    auto full_data = data;
                    auto [t, r] = record(data);
                    if (t == type::request)
                    {
                        auto c = owner.target->brush.sbg();
                        reply.osc(ansi::osc_set_bgcolor, utf::fprint("rgb:%r%/%g%/%b%", utf::to_hex(c.chan.r),
                                                                                        utf::to_hex(c.chan.g),
                                                                                        utf::to_hex(c.chan.b)));
                    }
                    else if (t == type::rgbcolor)
                    {
                        owner.target->brush.sbg(r);
                    }
                    else notsupported(ansi::osc_set_bgcolor, full_data, data);
                };
                procs[ansi::osc_caret_color] = [&](view data) // ESC ] 12 ;rgb:00/00/00
                {
                    auto full_data = data;
                    auto [t, r] = record(data);
                    if (t == type::request)
                    {
                        auto c = owner.caret.bgc();
                        reply.osc(ansi::osc_caret_color, utf::fprint("rgb:%r%/%g%/%b%", utf::to_hex(c.chan.r),
                                                                                        utf::to_hex(c.chan.g),
                                                                                        utf::to_hex(c.chan.b)));
                    }
                    else if (t == type::rgbcolor)
                    {
                        owner.caret.bgc(r);
                    }
                    else notsupported(ansi::osc_caret_color, full_data, data);
                };
                procs[ansi::osc_reset_crclr] = [&](view /*data*/)
                {
                    owner.caret.color(owner.defcfg.def_curclr);
                };
                procs[ansi::osc_reset_fgclr] = [&](view /*data*/)
                {
                    owner.target->brush.sfg(0);
                };
                procs[ansi::osc_reset_bgclr] = [&](view /*data*/ )
                {
                    owner.target->brush.sbg(0);
                };
            }

            void set(text const& property, view data)
            {
                auto proc = procs.find(property);
                if (proc != procs.end())
                {
                    proc->second(data);
                    if (reply.size())
                    {
                        owner.answer(reply);
                        reply.clear();
                    }
                }
                else log("%%Not supported: OSC=%property% DATA=%data% HEX=%hexdata%", prompt::term, property, data, utf::buffer_to_hex(data));
            }
            void fgc(tint c) { owner.target->brush.fgc(color[c]); }
            void bgc(tint c) { owner.target->brush.bgc(color[c]); }
        };
