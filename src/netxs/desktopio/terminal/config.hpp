        // term: Terminal configuration.
        struct termconfig
        {
            using pals = std::remove_const_t<decltype(argb::vt256)>;

            si32 def_mxline;
            si32 def_length;
            si32 def_growdt;
            si32 def_growmx;
            wrap def_wrpmod;
            si32 def_tablen;
            si32 def_lucent;
            si32 def_margin;
            si32 def_border;
            si32 def_atexit;
            bool def_restart_cwd; // term: Use child's current working directory when restarting the session.
            cell def_curclr;
            argb def_fcolor;
            argb def_bcolor;
            argb def_filler;
            si32 def_selmod;
            si32 def_cursor;
            bool def_selalt;
            bool def_cur_on;
            bool resetonkey;
            bool resetonout;
            bool def_io_log;
            bool allow_logs;
            span def_period;
            pals def_colors;

            cell def_safe_c;
            cell def_ansi_c;
            cell def_rich_c;
            cell def_html_c;
            cell def_text_c;
            cell def_none_c;
            cell def_find_c;

            si32 def_safe_f;
            si32 def_ansi_f;
            si32 def_rich_f;
            si32 def_html_f;
            si32 def_text_f;
            si32 def_none_f;
            si32 def_find_f;

            bool def_alt_on;

            text send_input;

            utf::unordered_map<text, ui32> color_names;

            static void recalc_buffer_metrics(si32& def_length, si32& def_growdt, si32& def_growmx)
            {
                if (def_growdt == 0)
                {
                    if (def_length == 0) def_length = def_growmx;
                    else                 def_growmx = def_length;
                }
                if (def_growmx == 0 && def_growdt != 0)
                {
                    def_growmx = std::exchange(def_length, def_growdt);
                }
            }
            termconfig(settings& config)
            {
                static auto atexit_options = utf::unordered_map<text, commands::atexit::codes>
                    {{ "auto",    commands::atexit::smart   },
                     { "ask",     commands::atexit::ask     },
                     { "close",   commands::atexit::close   },
                     { "restart", commands::atexit::restart },
                     { "retry",   commands::atexit::retry   }};
                static auto fx_options = utf::unordered_map<text, commands::fx::shader>
                    {{ "xlight",  commands::fx::xlight  },
                     { "coolor",  commands::fx::color   },
                     { "invert",  commands::fx::invert  },
                     { "reverse", commands::fx::reverse }};

                send_input =             config.settings::take("/config/terminal/sendinput",                 text{});
                def_mxline = std::max(1, config.settings::take("/config/terminal/scrollback/maxline",        si32{ 65535 }));
                def_length = std::max(1, config.settings::take("/config/terminal/scrollback/size",           si32{ 40000 }));
                def_growdt = std::max(0, config.settings::take("/config/terminal/scrollback/growstep",       si32{ 0 }    ));
                def_growmx = std::max(0, config.settings::take("/config/terminal/scrollback/growlimit",      si32{ 0 }    ));
                recalc_buffer_metrics(def_length, def_growdt, def_growmx);
                def_wrpmod =             config.settings::take("/config/terminal/scrollback/wrap",            deco::defwrp == wrap::on) ? wrap::on : wrap::off;
                resetonkey =             config.settings::take("/config/terminal/scrollback/reset/onkey",     true);
                resetonout =             config.settings::take("/config/terminal/scrollback/reset/onoutput",  faux);
                def_alt_on =             config.settings::take("/config/terminal/scrollback/altscroll",       true);
                def_lucent = std::max(0, config.settings::take("/config/terminal/scrollback/oversize/opacity",si32{ 0xC0 } ));
                def_margin = std::max(0, config.settings::take("/config/terminal/scrollback/oversize",        si32{ 0 }    ));
                def_tablen = std::max(1, config.settings::take("/config/terminal/tablen",                     si32{ 8 }    ));
                def_border = std::max(0, config.settings::take("/config/terminal/border",                     si32{ 0 }    ));
                def_selmod =             config.settings::take("/config/terminal/selection/mode",             mime::textonly, xml::options::format);
                def_selalt =             config.settings::take("/config/terminal/selection/rect",             faux);
                def_cur_on =             config.settings::take("/config/cursor/show",                     true);
                def_cursor =             config.settings::take("/config/cursor/style",                    text_cursor::I_bar, xml::options::cursor);
                def_curclr =             config.settings::take("/config/cursor/color",                    cell{});
                def_period =             config.settings::take("/config/cursor/blink",                    span{ skin::globals().blink_period });
                def_io_log =             config.settings::take("/config/debug/logs",        faux);
                allow_logs =             true; // Disallowed for dtty.
                def_atexit =             config.settings::take("/config/terminal/atexit",                     commands::atexit::smart, atexit_options);
                def_restart_cwd =        config.settings::take("/config/terminal/restart_cwd",                faux);
                def_fcolor =             config.settings::take("/config/terminal/colors/default/fgc",         argb{ whitelt });
                def_bcolor =             config.settings::take("/config/terminal/colors/default/bgc",         argb{ blackdk });
                def_filler =             config.settings::take("/config/terminal/colors/bground",             argb{ argb::default_color });

                def_safe_c =             config.settings::take("/config/terminal/colors/selection/protected", cell{}.bgc(bluelt)    .fgc(whitelt));
                def_ansi_c =             config.settings::take("/config/terminal/colors/selection/ansi",      cell{}.bgc(bluelt)    .fgc(whitelt));
                def_rich_c =             config.settings::take("/config/terminal/colors/selection/rich",      cell{}.bgc(bluelt)    .fgc(whitelt));
                def_html_c =             config.settings::take("/config/terminal/colors/selection/html",      cell{}.bgc(bluelt)    .fgc(whitelt));
                def_text_c =             config.settings::take("/config/terminal/colors/selection/text",      cell{}.bgc(bluelt)    .fgc(whitelt));
                def_none_c =             config.settings::take("/config/terminal/colors/selection/none",      cell{}.bgc(blacklt)   .fgc(whitedk));
                def_find_c =             config.settings::take("/config/terminal/colors/match",               cell{}.bgc(0xFF007F00).fgc(whitelt));

                def_safe_f =             config.settings::take("/config/terminal/colors/selection/protected/fx", commands::fx::color,  fx_options);
                def_ansi_f =             config.settings::take("/config/terminal/colors/selection/ansi/fx",      commands::fx::xlight, fx_options);
                def_rich_f =             config.settings::take("/config/terminal/colors/selection/rich/fx",      commands::fx::xlight, fx_options);
                def_html_f =             config.settings::take("/config/terminal/colors/selection/html/fx",      commands::fx::xlight, fx_options);
                def_text_f =             config.settings::take("/config/terminal/colors/selection/text/fx",      commands::fx::color,  fx_options);
                def_none_f =             config.settings::take("/config/terminal/colors/selection/none/fx",      commands::fx::color,  fx_options);
                def_find_f =             config.settings::take("/config/terminal/colors/match/fx",               commands::fx::color,  fx_options);

                {
                    auto color_names_context = config.settings::push_context("/config/terminal/colors/names");
                    auto color_name_ptr_list = config.settings::take_ptr_list_for_name("name");
                    for (auto& name_ptr : color_name_ptr_list)
                    {
                        auto name = utf::name2token(config.take_value(name_ptr));
                        auto rgba = config.take_value_from(name_ptr, "rgb", argb{});
                        color_names[name] = rgba.token;
                    }
                }

                std::copy(std::begin(argb::vt256), std::end(argb::vt256), std::begin(def_colors));
                for (auto i = 0; i < 16; i++)
                {
                    def_colors[i] = config.settings::take("/config/terminal/colors/color" + std::to_string(i), def_colors[i]);
                }
            }
        };
