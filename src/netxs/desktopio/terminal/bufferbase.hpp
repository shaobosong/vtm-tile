        // term: Generic terminal buffer.
        struct bufferbase
            : public ansi::parser
        {
            static void set_autocr(bool autocr)
            {
                #define V []([[maybe_unused]] auto& q, [[maybe_unused]] auto& p)
                auto& parser = ansi::get_parser<bufferbase>();
                autocr ? parser.intro[ansi::ctrl::eol] = V{ p->hard_lf(q.pop_all(ansi::ctrl::eol), true); }
                       : parser.intro[ansi::ctrl::eol] = V{ p->hard_lf(q.pop_all(ansi::ctrl::eol));       };
                #undef V
            }
            // bufferbase: Register buffer-level VT command handlers (p->method()).
            template<class T>
            static void register_buffer_ops(T& vt)
            {
                using namespace netxs::ansi;
                #define V []([[maybe_unused]] auto& q, [[maybe_unused]] auto& p)
                vt.csier.table_space[csi_spc_src] = V{ p->na("CSI n SP A  Shift right n columns(s)"); }; // CSI n SP A
                vt.csier.table_space[csi_spc_slc] = V{ p->na("CSI n SP @  Shift left  n columns(s)"); }; // CSI n SP @
                vt.csier.table_hash [csi_hsh_scp] = V{ p->na("CSI n # P  Push current palette colors onto stack, n default is 0"); };
                vt.csier.table_hash [csi_hsh_rcp] = V{ p->na("CSI n # Q  Pop  current palette colors onto stack, n default is 0"); };
                vt.csier.table_hash [csi_hsh_psh] = V{ p->pushsgr(); }; // CSI # {
                vt.csier.table_hash [csi_hsh_pop] = V{ p->popsgr();  }; // CSI # }
                vt.csier.table_dollarsn[csi_dlr_fra] = V{ p->fra(q); }; // CSI Char ; Top ; Left ; Bottom ; Right $ x  (DECFRA).
                vt.csier.sgr_unsupported = [](si32 code, bufferbase*&)
                {
                    log("%%SGR %val% attribute is not supported", prompt::term, code);
                };
                vt.csier.table[csi_cuu]  = V{ p-> up(q(1)); }; // CSI n A  (CUU)
                vt.csier.table[csi_cud]  = V{ p-> dn(q(1)); }; // CSI n B  (CUD)
                vt.csier.table[csi_cuf]  = V{ p->cuf(q(1)); }; // CSI n C  (CUF)
                vt.csier.table[csi_cub]  = V{ p->cub(q(1)); }; // CSI n D  (CUB)
                vt.csier.table[csi_cud2] = V{ p-> dn(q(1)); }; // CSI n e  (VPR)
                vt.csier.table[csi_cnl]  = V{ p->cr(); p->dn(q(1)); }; // CSI n E
                vt.csier.table[csi_cpl]  = V{ p->cr(); p->up(q(1)); }; // CSI n F
                vt.csier.table[csi_chx]  = V{ p->chx( q(1)); }; // CSI n G
                vt.csier.table[csi_chy]  = V{ p->chy( q(1)); }; // CSI n d
                vt.csier.table[csi_cup]  = V{ p->cup( q   ); }; // CSI y ; x H
                vt.csier.table[csi_hvp]  = V{ p->cup( q   ); }; // CSI y ; x f
                vt.csier.table[csi_cht]           = V{ p->tab( q(1)); }; // CSI n I
                vt.csier.table[csi_cbt]           = V{ p->tab(-q(1)); }; // CSI n Z
                vt.csier.table[csi_tbc]           = V{ p->tbc( q(0)); }; // CSI n g
                vt.csier.table[csi_rep]           = V{ p->rep( q(1)); }; // CSI n b
                vt.csier.table_quest[csi_qst_rtb] = V{ p->rtb(     ); }; // CSI ? W
                vt.intro[ctrl::esc][esc_hts]      = V{ p->stb(     ); }; // ESC H
                vt.csier.table[csi_dch] = V{ p->dch( q(1)); };  // CSI n P  (DCH)
                vt.csier.table[csi_ech] = V{ p->ech( q(1)); };  // CSI n X  (ECH)
                vt.csier.table[csi_ich] = V{ p->ins( q(1)); };  // CSI n @  (ICH)
                vt.csier.table[csi__ed] = V{ p-> ed( q(0)); }; // CSI n J
                vt.csier.table[csi__el] = V{ p-> el( q(0)); }; // CSI n K
                vt.csier.table[csi__il] = V{ p-> il( q(1)); }; // CSI n L  (IL)
                vt.csier.table[csi__dl] = V{ p-> dl( q(1)); }; // CSI n M  (DL)
                vt.csier.table[csi__sd] = V{ p->scl( q(1)); }; // CSI n T
                vt.csier.table[csi__su] = V{ p->scl(-q(1)); }; // CSI n S
                vt.csier.table[csi_scp] = V{ p->scp(     ); }; // CSI   s
                vt.csier.table[csi_rcp] = V{ p->rcp(     ); }; // CSI   u
                vt.csier.table[decstbm] = V{ p->scr( q   ); }; // CSI r; b r  (DECSTBM)
                vt.csier.table[csi_ccc][ccc_pad] = V{ p->setpad(q.subarg(-1)); }; // CCC_PAD
                vt.intro[ctrl::esc][esc_ind   ] = V{ p->lf(1); };             // ESC D  (IND)
                vt.intro[ctrl::esc][esc_ir    ] = V{ p->ri();  };             // ESC M  (RI)
                vt.intro[ctrl::esc][esc_sc    ] = V{ p->scp(); };             // ESC 7
                vt.intro[ctrl::esc][esc_rc    ] = V{ p->rcp(); };             // ESC 8
                vt.intro[ctrl::esc][esc_nel   ] = V{ p->hard_lf(1, true); };  // ESC E  (NEL)
                vt.intro[ctrl::esc][esc_decdhl] = V{ p->dhl(q); };            // ESC # ...
                vt.intro[ctrl::esc][esc_apc   ] = V{ p->apc(q); };            // ESC _ ... ST  (APC)
                vt.intro[ctrl::esc][esc_dcs   ] = V{ p->msg(esc_dcs, q); };   // ESC P ... ST  (DCS)
                vt.intro[ctrl::esc][esc_sos   ] = V{ p->msg(esc_sos, q); };   // ESC X ... ST  (SOS)
                vt.intro[ctrl::esc][esc_pm    ] = V{ p->msg(esc_pm , q); };   // ESC ^ ... ST  (PM)
                vt.intro[ctrl::bs ] = V{ p->cub(q.pop_all(ctrl::bs )); };
                vt.intro[ctrl::del] = V{ p->del(q.pop_all(ctrl::del)); };
                vt.intro[ctrl::tab] = V{ p->tab(q.pop_all(ctrl::tab)); };
                vt.intro[ctrl::eol] = V{ p->hard_lf(q.pop_all(ctrl::eol)); }; // LF
                vt.intro[ctrl::vt ] = V{ p->hard_lf(q.pop_all(ctrl::vt )); }; // VT
                vt.intro[ctrl::ff ] = V{ p->hard_lf(q.pop_all(ctrl::ff )); }; // FF
                vt.intro[ctrl::cr ] = V{ p->cr();                         };   // CR
                #undef V
            }
            // bufferbase: Register term-level VT command handlers (p->owner.method()).
            template<class T>
            static void register_term_ops(T& vt)
            {
                using namespace netxs::ansi;
                #define V []([[maybe_unused]] auto& q, [[maybe_unused]] auto& p)
                vt.csier.table_excl [csi_exl_rst]    = V{ p->owner.decstr( ); };   // CSI ! p  (DECSTR)
                vt.intro[ctrl::esc][esc_ris        ] = V{ p->owner.decstr(); };     // ESC c  (RIS)
                vt.csier.table_dollarsn[csi_dlr_cra] = V{ p->owner.deccra(q); };    // CSI ... $ v  (DECCRA)
                vt.csier.table_quest[dec_set] = V{ p->owner.decset(q); };           // CSI ? n h
                vt.csier.table_quest[dec_rst] = V{ p->owner.decrst(q); };           // CSI ? n l
                vt.csier.table[dec_set] = V{ p->owner.modset(q); };                 // CSI n h
                vt.csier.table[dec_rst] = V{ p->owner.modrst(q); };                 // CSI n l
                vt.csier.table[csi_ccc][ccc_sbs] = V{ p->owner.sbsize(q); };                      // CCC_SBS
                vt.csier.table[csi_ccc][ccc_rst] = V{ p->owner.setdef();  };                      // CCC_RST
                vt.csier.table[csi_ccc][ccc_sgr] = V{ p->owner.setsgr(q); };                      // CCC_SGR
                vt.csier.table[csi_ccc][ccc_lsr] = V{ p->owner.setlsr(q.subarg(1)); };            // CCC_LSR
                vt.csier.table[csi_ccc][ccc_sel] = V{ p->owner.selection_selmod(q.subarg(0)); };   // CCC_SEL
                vt.oscer[osc_clipboard  ] = V{ p->owner.forward_clipboard(q); };
                vt.oscer[osc_term_notify] = V{ p->owner.osc_notify(q);        };
                #undef V
            }
            // bufferbase: Register tracker-level VT command handlers (p->owner.{wtrack,ctrack,caret}).
            template<class T>
            static void register_tracker_ops(T& vt)
            {
                using namespace netxs::ansi;
                #define V []([[maybe_unused]] auto& q, [[maybe_unused]] auto& p)
                vt.csier.table_space[csi_spc_cst] = V{ p->owner.caret.decscusr(q(1)); }; // CSI n SP q  (DECSCUSR)
                vt.csier.sgr_palette_color = [](bufferbase*& p, byte index) -> argb
                {
                    return p->owner.ctrack.color[index];
                };
                vt.csier.table[csi_win] = V{ p->owner.wtrack.manage(q   ); }; // CSI n;m;k t  (XTWINOPS)
                vt.csier.table[csi_dsr] = V{ p->owner.wtrack.report(q(6)); }; // CSI n n  (DSR)
                vt.csier.table[csi_pda] = V{ p->owner.wtrack.device(q(0)); }; // CSI n c  (Primary DA)
                vt.oscer[osc_label_title] = V{ p->owner.wtrack.set(osc_label_title, q); };
                vt.oscer[osc_label      ] = V{ p->owner.wtrack.set(osc_label,       q); };
                vt.oscer[osc_title      ] = V{ p->owner.wtrack.set(osc_title,       q); };
                vt.oscer[osc_xprop      ] = V{ p->owner.wtrack.set(osc_xprop,       q); };
                vt.oscer[osc_linux_color] = V{ p->owner.ctrack.set(osc_linux_color, q); };
                vt.oscer[osc_linux_reset] = V{ p->owner.ctrack.set(osc_linux_reset, q); };
                vt.oscer[osc_set_palette] = V{ p->owner.ctrack.set(osc_set_palette, q); };
                vt.oscer[osc_set_fgcolor] = V{ p->owner.ctrack.set(osc_set_fgcolor, q); };
                vt.oscer[osc_set_bgcolor] = V{ p->owner.ctrack.set(osc_set_bgcolor, q); };
                vt.oscer[osc_reset_color] = V{ p->owner.ctrack.set(osc_reset_color, q); };
                vt.oscer[osc_caret_color] = V{ p->owner.ctrack.set(osc_caret_color, q); };
                vt.oscer[osc_reset_fgclr] = V{ p->owner.ctrack.set(osc_reset_fgclr, q); };
                vt.oscer[osc_reset_bgclr] = V{ p->owner.ctrack.set(osc_reset_bgclr, q); };
                vt.oscer[osc_reset_crclr] = V{ p->owner.ctrack.set(osc_reset_crclr, q); };
                #undef V
            }
            // bufferbase: Register all VT command handlers and fill unimplemented slots with logging stubs.
            template<class T>
            static void parser_config(T& vt)
            {
                register_buffer_ops(vt);
                register_term_ops(vt);
                register_tracker_ops(vt);

                // Log all unimplemented CSI commands.
                for (auto i = 0; i < 0x100; ++i)
                {
                    auto& proc = vt.csier.table[i];
                    if (!proc)
                    {
                        proc = [i](auto& q, auto& p){ p->not_implemented_CSI(i, q); };
                    }
                }
                auto& esc_lookup = vt.intro[netxs::ansi::ctrl::esc];
                // Log all unimplemented ESC+rest.
                for (auto i = 0; i < 0x100; ++i)
                {
                    auto& proc = esc_lookup[i];
                    if (!proc)
                    {
                        proc = [i](auto& q, auto& p){ p->not_implemented_ESC(i, q); };
                    }
                }
            }

            using tabs = std::vector<std::pair<si32, si32>>; // Pairs of forward and reverse tabstops index.

            struct line
                : public rich
            {
                using rich::rich;
                using type = deco::type;
                using id_t = ui32;

                line(line&& l)
                    : rich { std::forward<rich>(l) },
                      index{ l.index }
                {
                    style = l.style;
                    _size = l._size;
                    _kind = l._kind;
                    fill = l.fill;
                    tail = l.tail;
                    l._size = {};
                    l._kind = {};
                    l.fill = no_fill;
                }
                line(line const& l)
                    : rich{ l       },
                     index{ l.index },
                     style{ l.style },
                      fill{ l.fill  },
                      tail{ l.tail  }
                { }
                line(id_t line_id, deco const& line_style, span dt, twod sz)
                    : rich{ dt, sz     },
                     index{ line_id    },
                     style{ line_style }
                { }
                line(id_t line_id, deco const& line_style, cell const& blank)
                    : rich{ blank      },
                     index{ line_id    },
                     style{ line_style }
                { }
                line(id_t line_id, deco const& line_style, cell const& blank, si32 length)
                    : rich{ blank, length },
                     index{ line_id       },
                     style{ line_style    }
                { }
                line(core&& s)
                    : rich{ std::forward<core>(s) }
                { }
                line(netxs::view utf8)
                    : rich{ para{ utf8 }.content() }
                { }

                line& operator = (line&&)      = default;
                line& operator = (line const&) = default;

                id_t index{};
                deco style{};
                si32 _size{};
                type _kind{};
                static constexpr auto no_fill = -1;
                si32 fill{ no_fill };
                cell tail{};

                friend void swap(line& lhs, line& rhs)
                {
                    std::swap<rich>(lhs, rhs);
                    std::swap(lhs.index, rhs.index);
                    std::swap(lhs.style, rhs.style);
                    std::swap(lhs._size, rhs._size);
                    std::swap(lhs._kind, rhs._kind);
                    std::swap(lhs.fill, rhs.fill);
                    std::swap(lhs.tail, rhs.tail);
                }
                void wipe()
                {
                    rich::kill();
                    _size = {};
                    _kind = {};
                    fill = no_fill;
                    tail = {};
                }
                bool wrapped() const
                {
                    assert(_kind == style.get_kind());
                    return _kind == type::autowrap;
                }
                bool fills_eol() const
                {
                    return fill != no_fill;
                }
                auto fill_from() const
                {
                    return fill;
                }
                auto const& fill_cell() const
                {
                    return tail;
                }
                void fill_to_eol(si32 start, cell const& blank)
                {
                    fill = start;
                    tail = blank;
                }
                void reset_fill()
                {
                    fill = no_fill;
                    tail = {};
                }
                si32 height(si32 panel_x) const
                {
                    auto len = length();
                    assert(_kind == style.get_kind());
                    return len > panel_x && wrapped() ? (len + panel_x - 1) / panel_x
                                                      : 1;
                }
            };
            struct redo
            {
                using mark = ansi::mark;
                using sgrs = std::list<mark>;

                deco style{}; // Parser style state.
                mark brush{}; // Parser brush state.
                si32 decsg{}; // Parser DEC Special Graphcs Mode.
                twod coord{}; // Screen coord state.
                bool decom{}; // Origin mode  state.
                sgrs stack{}; // Stach for saved sgr attributes.
            };

            term& owner; // bufferbase: Terminal object reference.
            twod  panel; // bufferbase: Viewport size.
            twod  coord; // bufferbase: Viewport cursor position; 0-based.
            redo  saved; // bufferbase: Saved cursor position and rendition state.
            si32  arena; // bufferbase: Scrollable region height.
            si32  sctop; // bufferbase: Precalculated scrolling region top    height.
            si32  scend; // bufferbase: Precalculated scrolling region bottom height.
            si32  y_top; // bufferbase: Precalculated 0-based scrolling region top    vertical pos.
            si32  y_end; // bufferbase: Precalculated 0-based scrolling region bottom vertical pos.
            si32  n_top; // bufferbase: Original      1-based scrolling region top    vertical pos (use 0 if it is not set).
            si32  n_end; // bufferbase: Original      1-based scrolling region bottom vertical pos (use 0 if it is not set).
            tabs  stops; // bufferbase: Tabstop index.
            bool  notab; // bufferbase: Tabstop index is cleared.
            bool  decom; // bufferbase: Origin mode.

            bool  boxed; // bufferbase: Box selection mode.
            bool  grant; // bufferbase: Is it allowed to change box selection mode.
            bool  uirev; // bufferbase: Prev button highlighted.
            bool  uifwd; // bufferbase: Next button highlighted.
            ui64  alive; // bufferbase: Selection is active (digest).
            line  match; // bufferbase: Search pattern for highlighting.

            rich  tail_frag; // bufferbase: IRM cached fragment.
            rich  char_2d; // bufferbase: 2D char image.

            bufferbase(term& master)
                : owner{ master },
                  panel{ dot_11 },
                  coord{ dot_00 },
                  arena{ 1      },
                  sctop{ 0      },
                  scend{ 0      },
                  y_top{ 0      },
                  y_end{ 0      },
                  n_top{ 0      },
                  n_end{ 0      },
                  notab{ faux   },
                  decom{ faux   },
                  boxed{ faux   },
                  grant{ faux   },
                  uirev{ faux   },
                  uifwd{ faux   },
                  alive{ 0      }
            {
                parser::style = ansi::def_style;
            }

            // bufferbase: Make a viewport screen copy.
            virtual void do_viewport_copy(face& dest) = 0;

            virtual void selection_create(twod coor, bool mode)           = 0;
            virtual bool selection_extend(twod coor, bool mode)           = 0;
            virtual void selection_follow(twod coor, bool lock)           = 0;
            virtual void selection_byword(twod coor)                      = 0;
            virtual void selection_byword(twod from, twod to)             = 0;
            virtual void selection_byline(twod coor)                      = 0;
            virtual void selection_byline(twod from, twod to)             = 0;
            virtual void selection_drag_word_start(twod coor)             = 0;
            virtual void selection_drag_word_pull(twod coor)              = 0;
            virtual void selection_drag_line_start(twod coor)             = 0;
            virtual void selection_drag_line_pull(twod coor)              = 0;
            virtual void selection_drag_clear()                           = 0;
            virtual void selection_selall()                               = 0;
            virtual text selection_pickup(si32 selmod)                    = 0;
            virtual void selection_render(face& dest)                     = 0;
            virtual void selection_status(term_state& status) const       = 0;
            virtual twod selection_gonext(feed direction)                 = 0;
            virtual twod selection_gofind(feed direction, view data = {}) = 0;
            virtual twod selection_search(feed direction, view data = {})
            {
                auto delta = dot_00;
                if (data.empty()) // Find next selection match.
                {
                    if (match.length())
                    {
                        delta = selection_gonext(direction);
                    }
                }
                else
                {
                    delta = selection_gofind(direction, data);
                }
                return delta;
            }
            virtual void selection_setjet(bias /*align*/ = {})
            {
                // Do nothing by default.
            }
            virtual void selection_setwrp(wrap /*wrapping*/ = {})
            {
                // Do nothing by default.
            }
            // bufferbase: Cancel text selection.
            virtual bool selection_cancel()
            {
                auto active = alive;
                if (alive)
                {
                    alive = {};
                    match = {};
                }
                return active;
            }
            // bufferbase: Set selection mode lock state.
            void selection_locked(bool lock)
            {
                grant = !lock;
            }
            // bufferbase: Return selection mode lock state.
            auto selection_locked()
            {
                return !grant;
            }
            // bufferbase: Ping selection state.
            virtual void selection_update(bool despace = true)
            {
                if (despace) // Exclude whitespce.
                {
                    auto nothing = match.each([](auto& c){ return !c.isspc(); });
                    if (nothing) match = {};
                }
                alive = datetime::uniqueid();
            }
            // bufferbase: Ping selection state if is available.
            void selection_review()
            {
                if (alive) ++alive;
            }
            // bufferbase: Return true if selection is active.
            auto selection_active() const
            {
                return alive;
            }
            // bufferbase: Set selection mode (boxed = true).
            void selection_selbox(bool new_state)
            {
                if (new_state) grant = true;
                if (grant) boxed = new_state;
            }
            // bufferbase: Return selection mode.
            bool selection_selbox() const
            {
                return boxed;
            }

            //virtual text get_current_line()                                             = 0;
            virtual cell cell_under_cursor()                                            = 0;
            virtual void scroll_region(si32 top, si32 end, si32 n, bool use_scrollback) = 0;
            virtual bool recalc_pads(dent& oversz)                                      = 0;
            virtual void output(face& canvas)                                           = 0;
            virtual si32 height()                                                       = 0;
            virtual void del_above()                                                    = 0;
            virtual void del_below()                                                    = 0;
            virtual si32 get_size() const                                               = 0;
            virtual si32 get_peak() const                                               = 0;
            virtual si32 get_step() const                                               = 0;
            virtual si32 get_mxsz() const                                               = 0;
                    auto get_view() const { return panel; }
                    auto get_mode() const { return !selection_active() ? term_state::type::empty:
                                                    selection_selbox() ? term_state::type::block:
                                                                         term_state::type::lines; }
            // bufferbase: Get viewport position.
    virtual si32 get_origin(bool /*follow*/)
            {
                return 0;
            }
            // bufferbase: Get viewport basis.
    virtual si32 get_basis()
            {
                return 0;
            }
            // bufferbase: Get viewport position.
    virtual si32 get_slide()
            {
                return 0;
            }
            // bufferbase: Set viewport position and return whether the viewport is reset.
    virtual bool set_slide(si32&)
            {
                return true;
            }
            // bufferbase: Set left/right scrollback additional padding.
    virtual void setpad(si32 /*new_value*/)
            { }
            // bufferbase: Get left/right scrollback additional padding.
    virtual si32 getpad()
            {
                return 0;
            }
            // bufferbase: Update scrolling region.
            void update_region()
            {
                sctop = std::max(0, n_top - 1);
                scend = n_end != 0 ? std::max(1, panel.y - n_end)
                                   : 0;
                auto y_max = panel.y - 1;
                y_end = std::clamp(y_max - scend, 0, y_max);
                y_top = std::clamp(sctop        , 0, y_end);
            }
            // bufferbase: Resize viewport.
    virtual void resize_viewport(twod new_sz, bool /*forced*/ = faux)
            {
                panel = std::max(new_sz, dot_11);
                resize_tabstops(panel.x);
                update_region();
                selection_review();
                arena = y_end - y_top + 1; // Can be changed at the scrollbuff::set_scroll_region(si32 top, si32 bottom).
            }
            // bufferbase: Reset coord and set the scrolling region using 1-based top and bottom. Use 0 to reset.
    virtual void set_scroll_region(si32 top, si32 bottom)
            {
                top    = std::clamp(top,    0, panel.y);
                bottom = std::clamp(bottom, 0, panel.y);
                if (top != 0 && bottom != 0 && top >= bottom) // && top > bottom) top = bottom; //todo Nobody respects that.
                {
                    top = bottom = 0;
                }
                n_top = top    == 1       ? 0 : top;
                n_end = bottom == panel.y ? 0 : bottom;
                update_region();
                cup0(dot_00);
            }
            // bufferbase: Set cursor position.
    virtual void set_coord(twod new_coord)
            {
                coord = new_coord;
            }
            // bufferbase: Return current 0-based cursor position in the viewport.
    virtual twod get_coord(twod /*origin*/)
            {
                return coord;
            }
            // bufferbase: Base-CSI contract (see ansi::csi_t).
            //             task(...), meta(...), data(...)
            void task(ansi::rule property)
            {
                parser::flush();
                log("%%DirectVT extensions are not supported: arg=%%, cmd=%%", prompt::term, property.arg, property.cmd);
                //auto& cur_line = batch.current();
                //if (cur_line.busy())
                //{
                //    add_lines(1);
                //    batch.index(batch.length() - 1);
                //}
                //batch->locus.push(property);
            }
            // bufferbase: Update current SGR attributes.
    virtual void meta(deco const& old_style) override
            {
                auto changed = faux;
                if (parser::style.wrp() != old_style.wrp())
                {
                    auto w = parser::style.wrp() == wrap::none ? (si32)owner.defcfg.def_wrpmod
                                                               : (si32)parser::style.wrp();
                    owner.base::signal(tier::release, terminal::events::layout::wrapln, w);
                    changed = true;
                }
                if (parser::style.jet() != old_style.jet())
                {
                    auto a = parser::style.jet() == bias::none ? (si32)bias::left
                                                               : (si32)parser::style.jet();
                    owner.base::signal(tier::release, terminal::events::layout::align, a);
                    changed = true;
                }
                if (changed && owner.styled)
                {
                    owner.ipccon.style(parser::style, owner.kbmode);
                }
            }
            template<class T>
            void na(T&& note)
            {
                log(prompt::term, "Not implemented: ", note);
            }
            void not_implemented_CSI(si32 i, fifo& q)
            {
                auto params = text{};
                while (q)
                {
                    params += std::to_string(q(0));
                    if (q)
                    {
                        auto is_sub_arg = q.issub(q.front());
                        auto delim = is_sub_arg ? ':' : ';';
                        params.push_back(delim);
                    }
                }
                log("%%CSI %params% %char%, (%val%) is not implemented", prompt::term, params, (byte)i, i);
            }
            void not_implemented_ESC(si32 c, qiew& q)
            {
                switch (c)
                {
                    // Unexpected
                    case ansi::esc_csi   :
                    case ansi::esc_ocs   :
                    case ansi::esc_dcs   :
                    case ansi::esc_sos   :
                    case ansi::esc_pm    :
                    case ansi::esc_apc   :
                    case ansi::esc_st    :
                        log("%%ESC %char% (%val%) is unexpected", prompt::term, (char)c, c);
                        break;
                    // Unsupported ESC + byte + rest
                    case ansi::esc_g0set :
                    case ansi::esc_g1set :
                    case ansi::esc_g2set :
                    case ansi::esc_g3set :
                    case ansi::esc_g1xset:
                    case ansi::esc_g2xset:
                    case ansi::esc_g3xset:
                    case ansi::esc_ctrl  :
                    case ansi::esc_decdhl:
                    case ansi::esc_chrset:
                    {
                        if (!q) log("%%ESC %char% (%val%) is incomplete", prompt::term, (char)c, c);
                        auto b = q.front();
                        q.pop_front();
                        switch (b)
                        {
                            case 'B':
                            case 'A':
                            case '0':
                            case '1':
                            case '2':
                            case '<':
                            case '4':
                            case '5':
                            case 'C':
                            case 'R':
                            case 'f':
                            case 'Q':
                            case 'K':
                            case 'Y':
                            case 'E':
                            case '6':
                            case 'Z':
                            case '7':
                            case 'H':
                            case '=':
                            case '>':
                            case '9':
                            case '`':
                            case 'U':
                                log("%%ESC %char% %char% (%val% %val%) is unsupported", prompt::term, (char)c, (char)b, c, b);
                                break;
                            case '%':
                            case '"':
                            {
                                if (q.size() < 2)
                                {
                                    if (q) q.pop_front();
                                    log("%%ESC %char% %char% (%val% %val%) is incomplete", prompt::term, (char)c, (char)b, c, b);
                                }
                                else
                                {
                                     auto d = q.front();
                                     q.pop_front();
                                     log("%%ESC %char% %char% %char% (%val% %val% %val%) is unsupported", prompt::term, (char)c, (char)b, (char)d, c, b, d);
                                }
                                break;
                            }
                            default:
                                log("%%ESC %char% %char% (%val% %val%) is unknown", prompt::term, (char)c, (char)b, c, b);
                                break;
                        }
                        break;
                    }
                    // Unsupported ESC + byte
                    case ansi::esc_delim :
                    case ansi::esc_key_a :
                    case ansi::esc_key_n :
                    case ansi::esc_decbi :
                    case ansi::esc_decfi :
                    case ansi::esc_sc    :
                    case ansi::esc_rc    :
                    case ansi::esc_hts   :
                    case ansi::esc_nel   :
                    case ansi::esc_clb   :
                    case ansi::esc_ind   :
                    case ansi::esc_ir    :
                    case ansi::esc_ris   :
                    case ansi::esc_memlk :
                    case ansi::esc_munlk :
                    case ansi::esc_ls2   :
                    case ansi::esc_ls3   :
                    case ansi::esc_ls1r  :
                    case ansi::esc_ls2r  :
                    case ansi::esc_ls3r  :
                    case ansi::esc_ss3   :
                    case ansi::esc_ss2   :
                    case ansi::esc_spa   :
                    case ansi::esc_epa   :
                    case ansi::esc_rid   :
                        log("%%ESC %char% (%val%) is unsupported", prompt::term, (char)c, c);
                        break;
                    default:
                        log("%%ESC %char% (%val%) is unknown", prompt::term, (char)c, c);
                        break;
                }
            }
            void dhl(qiew& q)
            {
                parser::flush();
                auto c = q ? q.front()
                           : -1;
                if (q) q.pop_front();
                switch (c)
                {
                    case -1:
                        log(prompt::term, "ESC #  is unexpected");
                        break;
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                        log("%%ESC # %char% (%val%) is unsupported", prompt::term, (char)c, c);
                        break;
                    case '8':
                    {
                        set_coord(dot_00);
                        auto y = 0;
                        while (++y <= panel.y) // Fill viewport with 'E'.
                        {
                            chy(y);
                            ech(panel.x, 'E');
                        }
                        set_coord(dot_00);
                        break;
                    }
                    default:
                        log("%%ESC # %char% (%val%) is unknown", prompt::term, (char)c, c);
                        break;
                }
            }
            void apc(qiew& q)
            {
                parser::flush();
                auto script_body = qiew{};
                auto head = q.begin();
                auto tail = q.end();
                while (head != tail)
                {
                    auto c = *head++;
                    if (c == ansi::c0_bel)
                    {
                        script_body = qiew{ q.begin(), std::prev(head) };
                        break;
                    }
                    else if (c == ansi::c0_esc && head != tail && *head == '\\')
                    {
                        script_body = qiew{ q.begin(), std::prev(head) };
                        head++;
                        break;
                    }
                }
                q = { head, tail };
                if (script_body.size() > ansi::apc_prefix_lua.size())
                {
                    auto payload_marker = text{ script_body.substr(0, ansi::apc_prefix_lua.size()) };
                    if (utf::to_lower(payload_marker) == ansi::apc_prefix_lua)
                    {
                        script_body.remove_prefix(ansi::apc_prefix_lua.size());
                        auto& luafx = owner.bell::indexer.luafx;
                        luafx.run_script(owner, script_body);
                    }
                    else
                    {
                        log("%%Unsupported APC payload: %payload%. Please use the '%lua%' prefix for the payload.", prompt::term, ansi::hi(utf::debase437(script_body)), ansi::apc_prefix_lua);
                    }
                }
            }
            void msg(si32 cmd, qiew& q)
            {
                parser::flush();
                auto data = text{};
                while (q)
                {
                    auto c = (char)q.front();
                    data.push_back(c);
                    q.pop_front();
                         if (c == ansi::c0_bel) break;
                    else if (c == ansi::c0_esc)
                    {
                        c = (char)q.front();
                        if (q && c == '\\')
                        {
                            data.push_back(c);
                            q.pop_front();
                            break;
                        }
                    }
                }
                log("%%Unsupported message/command: '\\e%char%%data%'", prompt::term, (char)cmd, utf::debase<faux>(data));
            }
            // bufferbase: Clear buffer.
    virtual void clear_all()
            {
                parser::state = {};
                parser::decsg = {};
                decom = faux;
                rtb();
                selection_cancel();
            }
            // tabstops index, tablen = 3, vector<pair<fwd_idx, rev_idx>>:
            // coor.x      -2-1 0 1 2 3 4 5 6 7 8 9
            // size = 9         0 1 2 3 4 5 6 7 8
            //             ----------------------
            // custom: fwd_idx  3 3 3 6 6 6 9 9 9
            //         rev_idx  0 0 0 3 3 3 6 6 6  coord.x - 1
            //
            // auto:   fwd_idx -3-3-3-6-6-6-9-9-9
            //         rev_idx  0 0 0 3 3 3 6 6 6  coord.x - 1
            //
            // empty:  fwd_idx -9-9-9-9-9-9-9-9-9
            //         rev_idx  0 0 0 0 0 0 0 0 0  coord.x - 1
            //
            // bufferbase: Clear tabstops.
            void clear_tabstops()
            {
                notab = true;
                auto auto_tabs = std::pair{ -panel.x, 0 }; // Negative means auto, not custom.
                stops.assign(panel.x, auto_tabs);
            }
            // bufferbase: Resize tabstop index.
            void resize_tabstops(si32 new_size, bool forced = faux)
            {
                auto size = (si32)stops.size();
                if (!forced && new_size <= size) return;

                auto last_stop = si32{};
                if (!stops.empty())
                {
                    auto back = stops.back();
                    auto last_size = back.first > 0 ? 0 // Custom tabstop -- don't touch it.
                                                    : -back.first - back.second;
                    last_stop = forced ? 0
                                       : size - last_size;
                    stops.resize(last_stop); // Trim.
                }

                if (notab) // Preserve existing tabstops.
                {
                    auto auto_tabs = std::pair{ -new_size, last_stop };
                    stops.resize(new_size, auto_tabs);
                }
                else // Add additional default tabstops.
                {
                    stops.reserve(new_size);
                    auto step = owner.defcfg.def_tablen;
                    auto next = last_stop / step * step;
                    auto add_count = new_size - step;
                    while (next < add_count)
                    {
                        auto prev = next;
                        next += step;
                        auto auto_tabs = std::pair{ -next, prev };
                        stops.resize(stops.size() + step, auto_tabs);
                    }
                    auto auto_tabs = std::pair{ -new_size, next };
                    stops.resize(new_size, auto_tabs);
                }
            }
            // bufferbase: ESC H  Place tabstop at the current cursor posistion.
            void stb()
            {
                parser::flush();
                if (coord.x <= 0 || coord.x > owner.defcfg.def_mxline) return;
                resize_tabstops(coord.x);
                auto  coor = coord.x - 1;
                auto  head = stops.begin();
                auto  tail = stops.begin() + coor;
                auto& last = tail->first;
                if (coord.x != last)
                {
                    auto size = (si32)stops.size();
                    auto base = last;
                    last = coord.x;
                    while (head != tail)
                    {
                        auto& tab = (--tail)->first;
                        if (tab == base) tab = coord.x;
                        else             break;
                    }
                    if (coord.x < size)
                    {
                        head += coord.x;
                        tail = stops.end();
                        auto& next = head->second;
                        auto  prev = next;
                        next = coord.x;
                        while (++head != tail)
                        {
                            auto& tab = head->second;
                            if (tab == prev) tab = coord.x;
                            else             break;
                        }
                    }
                }
            }
            // bufferbase: (see CSI 0 g) Remove tabstop at the current cursor posistion.
            void remove_tabstop()
            {
                auto  size = (si32)stops.size();
                if (coord.x <= 0 || coord.x >= size) return;
                auto  head = stops.begin();
                auto  tail = stops.begin() + coord.x;
                auto  back = tail;
                auto& stop = *tail;
                auto& item = *--tail;
                auto  main = stop.first;
                auto  base = std::abs(item.first);

                if (base == std::abs(main)) return;

                item.first = main;
                while (head != tail)
                {
                    --tail;
                    auto& tab = tail->first;
                    if (base == std::abs(tab)) tab = main;
                    else                       break;
                }

                tail = stops.end();
                base = stop.second;
                main = item.second;
                stop.second = main;
                while (++back != tail)
                {
                    auto& tab = back->second;
                    if (base == std::abs(tab)) tab = main;
                    else                       break;
                }
            }
            // bufferbase: CSI ? W  Reset tabstops to defaults.
            void rtb()
            {
                notab = faux;
                resize_tabstops(panel.x, true);
            }
            // bufferbase: Horizontal tab implementation.
            template<bool Fwd, class T>
            void tab_impl(T size)
            {
                if constexpr (Fwd)
                {
                    auto x = std::clamp(coord.x, 0, size ? size - 1 : 0);
                    if (coord.x == x)
                    {
                        coord.x = std::abs(stops[x].first);
                    }
                    else
                    {
                        coord.x += notab ? owner.defcfg.def_tablen
                                         : owner.defcfg.def_tablen - netxs::grid_mod(coord.x, owner.defcfg.def_tablen);
                    }
                }
                else
                {
                    auto x = std::clamp(coord.x, 1, size);
                    if (coord.x == x)
                    {
                        coord.x = stops[x - 1].second;
                    }
                    else
                    {
                        coord.x -= notab ? owner.defcfg.def_tablen
                                         :(owner.defcfg.def_tablen + coord.x - 1) % owner.defcfg.def_tablen + 1;
                    }
                }
            }
            // bufferbase: TAB  Horizontal tab.
    virtual void tab(si32 n)
            {
                parser::flush();
                auto size = (si32)stops.size();
                if (n > 0) while (n-- > 0) tab_impl<true>(size);
                else       while (n++ < 0) tab_impl<faux>(size);
            }
            void print_tabstops(text msg)
            {
                log(msg, ":\n", "index size = ", stops.size());
                auto i = 0u;
                auto data = utf::adjust("coor:", 5, " ", faux);
                while (i < stops.size()) data += utf::adjust(std::to_string(i++), 4, " ", true);
                data += '\n' + utf::adjust("fwd:", 5, " ", faux);
                for (auto [fwd, rev] : stops) data += utf::adjust(std::to_string(fwd), 4, " ", true);
                data += '\n' + utf::adjust("rev:", 5, " ", faux);
                for (auto [fwd, rev] : stops) data += utf::adjust(std::to_string(rev), 4, " ", true);
                log(data);
            }
            // bufferbase: CSI n g  Reset tabstop value.
            void tbc(si32 n)
            {
                switch (n)
                {
                    case 0: // Remove tab stop from the current column.
                        parser::flush();
                        remove_tabstop();
                        break;
                    case 3: // Clear all tab stops.
                        clear_tabstops();
                        break;
                    default: // Test: print tab stops.
                        print_tabstops("Tabstops index: `CSI " + std::to_string(n) + " g`");
                        break;
                }
            }
            // bufferbase: CSI n b  Repeat prev character n times.
            template<bool Flush = true>
            void rep(si32 n)
            {
                if constexpr (Flush) parser::flush();
                n = std::clamp<si32>(n, 0, si16max);
                if (n)
                {
                    auto c = cell{ parser::brush };
                    parser::assign(n, c);
                }
            }
            // bufferbase: CSI Char ; Top ; Left ; Bottom ; Right $ x  Fill rectangular area (DECFRA).
            void fra(fifo& q)
            {
                parser::flush();
                auto c = q(' ');
                auto t = q(1);
                auto l = q(1);
                auto b = q(panel.y);
                auto r = q(panel.x);
                if (t > b) t = b;
                if (l > r) l = r;
                l -= 1;
                t -= 1;
                auto area = rect{{ l, t }, { r - l, b - t }};
                area.trunc(panel);
                if (c < ' ') c = ' ';
                auto sym = utf::to_utf_from_code(c);
                auto tmp = parser::brush;
                parser::brush.txt(sym);
                auto [w, h, x, y] = parser::brush.whxy();
                if (w > 1)
                {
                    area.size.x /= w;
                }
                if (area)
                {
                    auto sav = coord;
                    while (area.size.y--)
                    {
                        set_coord(area.coor);
                        rep<faux>(area.size.x);
                        area.coor.y++;
                    }
                    set_coord(sav);
                }
                parser::brush = tmp;
            }
            // bufferbase: CSI # {  Push SGR attributes.
            void pushsgr()
            {
                parser::flush();
                saved.stack.push_back(parser::brush);
                if (saved.stack.size() == 10) saved.stack.pop_front();
            }
            // bufferbase: CSI # }  Pop SGR attributes.
            void popsgr()
            {
                parser::flush();
                if (saved.stack.size())
                {
                    parser::brush = saved.stack.back();
                    saved.stack.pop_back();
                }
            }
            // bufferbase: ESC 7 or CSI s  Save cursor position.
            void scp()
            {
                parser::flush();
                saved = { .style = parser::style,
                          .brush = parser::brush,
                          .decsg = parser::decsg,
                          .coord = coord,
                          .decom = decom };
                if (decom) saved.coord.y -= y_top;
                assert(saved.coord.y >= 0);
            }
            // bufferbase: ESC 8 or CSI u  Restore cursor position.
            void rcp()
            {
                parser::flush();
                decom = saved.decom;
                auto coor = saved.coord;
                if (decom) coor.y += y_top;
                set_coord(coor);
                parser::style = saved.style;
                parser::brush = saved.brush;
                parser::decsg = saved.decsg;
                parser::flush(); // Proceed new style.
            }
            // bufferbase: CSI n T/S  Scroll down/up, scrolled up lines are pushed to the scrollback buffer.
    virtual void scl(si32 n)
            {
                parser::flush();
                scroll_region(y_top, y_end, n, n > 0 ? faux : true);
            }
            // bufferbase: CSI n L  Insert n lines. Place cursor to the beginning of the current.
    virtual void il(si32 n)
            {
                parser::flush();
                // Works only if cursor is in the scroll region.
                // Inserts n lines at the current row and removes n lines at the scroll bottom.
                if (n > 0 && coord.y >= y_top
                          && coord.y <= y_end)
                {
                    scroll_region(coord.y, y_end, n, faux);
                    coord.x = 0;
                }
            }
            // bufferbase: CSI n M  Delete n lines. Place cursor to the beginning of the current.
    virtual void dl(si32 n)
            {
                parser::flush();
                // Works only if cursor is in the scroll region.
                // Deletes n lines at the current row and add n lines at the scroll bottom.
                if (n > 0 && coord.y >= y_top && coord.y <= y_end)
                {
                    scroll_region(coord.y, y_end, -n, faux);
                    coord.x = 0;
                }
            }
            // bufferbase: Reverse index with using scrollback.
    virtual void _ri(si32 n)
            {
                // Reverse index
                // - move cursor up if it is outside of scrolling region or below the top line of scrolling region.
                // - scroll down if cursor is on the top line of scroll region.
                auto new_coord_y = coord.y - n;
                if (new_coord_y < y_top && coord.y >= y_top)
                {
                    auto dy = y_top - new_coord_y;
                    scroll_region(y_top, y_end, dy, true);
                    coord.y = y_top;
                }
                else coord.y = std::clamp(new_coord_y, 0, panel.y - 1);
            }
            // bufferbase: ESC M  Reverse index.
    virtual void ri()
            {
                parser::flush();
                // Reverse index
                // - move cursor one line up if it is outside of scrolling region or below the top line of scrolling region.
                // - one line scroll down if cursor is on the top line of scroll region.
                if (coord.y != y_top)
                {
                    coord.y--;
                }
                else scroll_region(y_top, y_end, 1, faux); // vi does not expect scrollback data (use_scrollback = true).
            }
            // bufferbase: CSI t;b r  Set scrolling region (t/b: top+bottom).
            void scr(fifo& q)
            {
                auto top = q(0);
                auto end = q(0);
                set_scroll_region(top, end);
            }
            // bufferbase: CSI n @  ICH. Insert n blanks after cursor. Don't change cursor pos.
    virtual void ins(si32 n) = 0;
            // bufferbase: Shift left n columns(s).
            void shl(si32 n)
            {
                log("%%SHL(%n%) is not implemented", prompt::term, n);
            }
            // bufferbase: CSI n X  Erase/put n chars after cursor. Don't change cursor pos.
    virtual void ech(si32 n, char c = '\0') = 0;
            // bufferbase: CSI n P  Delete (not Erase) letters under the cursor.
    virtual void dch(si32 n) = 0;
            // bufferbase: '\x7F'  Delete characters backwards.
    virtual void del(si32 n) = 0;
            // bufferbase: Move cursor by n in line.
    virtual void move(si32 n) = 0;
            // bufferbase: Move cursor forward by n.
    virtual void cuf(si32 n)
            {
                if (n < 0) move(n);
                else
                {
                    parser::flush();
                    if (n == 0) n = 1;
                    coord.x += n;
                }
            }
            // bufferbase: Move cursor backward by n.
    virtual void cub(si32 n)
            {
                if (n < 0) move(-n);
                else
                {
                    parser::flush();
                    if (n == 0) n = 1;
                    else if (coord.x == panel.x && parser::style.wrp() == wrap::on && n > 0) ++n;
                    coord.x -= n;
                }
            }
            // bufferbase: Absolute horizontal cursor position (0-based).
    virtual void chx0(si32 n)
            {
                parser::flush();
                coord.x = n;
            }
            // bufferbase: CSI n G  Absolute horizontal cursor position (1-based).
    virtual void chx(si32 n)
            {
                parser::flush();
                coord.x = n - 1;
            }
            // bufferbase: Absolute vertical cursor position (0-based).
    virtual void chy0(si32 n)
            {
                parser::flush_data();
                if (decom) coord.y = std::clamp(n + y_top, y_top, y_end);
                else       coord.y = std::clamp(n, 0, panel.y - 1);
            }
            // bufferbase: CSI n d  Absolute vertical cursor position (1-based).
    virtual void chy(si32 n)
            {
                parser::flush_data();
                --n;
                if (decom) coord.y = std::clamp(n + y_top, y_top, y_end);
                else       coord.y = std::clamp(n, 0, panel.y - 1);
            }
            // bufferbase: Set caret position (0-based).
            void _cup(twod p)
            {
                coord.x = std::clamp(p.x, 0, panel.x - 1);
                if (decom) coord.y = std::clamp(p.y + y_top, y_top, y_end);
                else       coord.y = std::clamp(p.y, 0, panel.y - 1);
            }
            // bufferbase: CSI y; x H/F  Cursor position (1-based).
    virtual void cup(twod p)
            {
                parser::flush_data();
                _cup(p - dot_11);
            }
            // bufferbase: Cursor position (0-based).
    virtual void cup0(twod p)
            {
                parser::flush_data();
                _cup(p);
            }
            // bufferbase: Cursor position (0-based) w/o data_flush.
    virtual void cup2(twod p)
            {
                _cup(p);
            }
            // bufferbase: CSI y; x H/F  Cursor position (1-based).
    virtual void cup(fifo& q)
            {
                auto y = q(1);
                auto x = q(1);
                cup({ x, y });
            }
            // bufferbase: Move cursor up.
    virtual void up(si32 n)
            {
                parser::flush_data();
                if (n == 0) n = 1;
                auto new_coord_y = coord.y - n;
                if (new_coord_y < y_top && coord.y >= y_top)
                {
                    auto dy = y_top - new_coord_y;
                    scroll_region(y_top, y_end, dy, true);
                    coord.y = y_top;
                }
                else coord.y = std::clamp(new_coord_y, 0, panel.y - 1);
            }
            // bufferbase: Move cursor down.
    virtual void dn(si32 n)
            {
                parser::flush_data();
                if (n == 0) n = 1;
                auto new_coord_y = coord.y + n;
                if (new_coord_y > y_end && coord.y <= y_end)
                {
                    auto dy = new_coord_y - y_end;
                    scroll_region(y_top, y_end, -dy, true);
                    coord.y = y_end;
                }
                else coord.y = std::clamp(new_coord_y, 0, panel.y - 1);
            }
            // bufferbase: Move cursor back.
    virtual void _cub(si32 n)
            {
                coord.x -= n;
            }
            // bufferbase: Line feed. Index. Scroll region up if new_coord_y > end.
    virtual void _lf(si32 n)
            {
                auto new_coord_y = coord.y + n;
                if (new_coord_y > y_end && coord.y <= y_end)
                {
                    auto dy = y_end - new_coord_y;
                    scroll_region(y_top, y_end, dy, true);
                    coord.y = y_end;
                }
                else coord.y = std::clamp(new_coord_y, 0, panel.y - 1);
                if (coord.x < 0 || coord.x > panel.x) coord.x = 0; // Auto cr if the cursor is outside the viewport (to prevent infinitely long lines when autocr is disabled).
            }
            // bufferbase: Line feed. Index. Scroll region up if new_coord_y > end.
    virtual void lf(si32 n)
            {
                parser::flush_data();
                _lf(n);
            }
            // bufferbase: Text newline that may terminate a soft-wrap chain.
    virtual void hard_lf(si32 n, bool with_cr = faux)
            {
                if (with_cr) cr();
                lf(n);
            }
            // bufferbase: '\r'  CR Cursor return. Go to home of visible line instead of home of paragraph.
    virtual void cr()
            {
                parser::flush();
                coord.x = 0;
            }
            // bufferbase: CSI n J  Erase display.
            void ed(si32 n)
            {
                parser::flush();
                switch (n)
                {
                    case commands::erase::display::below: // n = 0 (default)  Erase viewport after cursor.
                        del_below();
                        break;
                    case commands::erase::display::above: // n = 1  Erase viewport before cursor.
                        del_above();
                        break;
                    case commands::erase::display::viewport: // n = 2  Erase viewport.
                        set_coord(dot_00);
                        ed(commands::erase::display::below);
                        break;
                    case commands::erase::display::scrollback: // n = 3  Erase scrollback.
                        clear_all();
                        break;
                    default:
                        break;
                }
            }
            // bufferbase: CSI n K  Erase line (don't move cursor).
    virtual void el(si32 n) = 0;
            // bufferbase: Helper to reset viewport horizontal position.
            static auto reset_viewport(si32 origin_x, si32 x, si32 panel_x)
            {
                if (origin_x != 0 || x != panel_x)
                {
                         if (x >= 0  &&   x < panel_x) origin_x = 0;
                    else if (x >= -origin_x + panel_x) origin_x = 0 - x + panel_x - 1;
                    else if (x <  -origin_x          ) origin_x = 0 - x;
                }
                return origin_x;
            };
            // bufferbase: Select shader.
            template<class P>
            void _shade(si32 fx, cell const& c, P work)
            {
                switch (fx)
                {
                    case commands::fx::color:   work(cell::shaders::color(c)); break;
                    case commands::fx::xlight:  work(cell::shaders::xlight);   break;
                    case commands::fx::invert:  work(cell::shaders::invert);   break;
                    case commands::fx::reverse: work(cell::shaders::reverse);  break;
                }
            }
            // bufferbase: Shade selection.
            template<class P>
            auto _shade_selection(si32 mode, P work)
            {
                switch (owner.ftrack ? mode : mime::disabled)
                {
                    case mime::ansitext: _shade(owner.defcfg.def_ansi_f, owner.defcfg.def_ansi_c, work); break;
                    case mime::richtext: _shade(owner.defcfg.def_rich_f, owner.defcfg.def_rich_c, work); break;
                    case mime::htmltext: _shade(owner.defcfg.def_html_f, owner.defcfg.def_html_c, work); break;
                    case mime::textonly: _shade(owner.defcfg.def_text_f, owner.defcfg.def_text_c, work); break;
                    case mime::safetext: _shade(owner.defcfg.def_safe_f, owner.defcfg.def_safe_c, work); break;
                    default:             _shade(owner.defcfg.def_none_f, owner.defcfg.def_none_c, work); break;
                }
            }
            // bufferbase: Rasterize selection with grips.
            void selection_raster(face& dest, auto curtop, auto curend, bool /*ontop*/ = true, bool /*onend*/ = true)
            {
                if (selection_active())
                {
                    auto mode = owner.selmod;
                    auto clip = dest.clip();
                    auto grip_1 = rect{ curtop, dot_11 };
                    auto grip_2 = rect{ curend, dot_11 };
                    grip_1.coor.x += clip.coor.x; // Compensate scrollback's hz movement.
                    grip_2.coor.x += clip.coor.x; //
                    auto square = grip_1 | grip_2;
                    square.normalize_itself();
                    auto work = [&](auto fill)
                    {
                        if (!selection_selbox())
                        {
                            auto size_0 = square.size - dot_01;
                            auto size_1 = curtop.x + curtop.y * panel.x;
                            auto size_2 = curend.x + curend.y * panel.x;
                            if (size_1 > size_2) std::swap(curtop, curend);
                            auto a = curtop.x;
                            auto b = curend.x + 1;
                            if (curtop.x > curend.x)
                            {
                                square.coor += dot_11;
                                square.size -= dot_11 + dot_11;
                                std::swap(a, b);
                            }
                            auto west = rect{{ 0, curtop.y + 1 }, { a,           size_0.y }}.normalize();
                            auto east = rect{{ b, curtop.y     }, { panel.x - b, size_0.y }}.normalize();
                            west.coor.x += clip.coor.x; // Compensate scrollback's hz movement.
                            east.coor.x += clip.coor.x; //
                            west.trimby(clip);
                            east.trimby(clip);
                            dest.fill(west, fill);
                            dest.fill(east, fill);
                        }
                        square.trimby(clip);
                        dest.fill(square, fill);
                    };
                    _shade_selection(mode, work);
                }
            }
            // bufferbase: Pickup selected data from canvas.
            void selection_pickup(escx& buffer, rich& canvas, twod seltop, twod selend, si32 selmod, bool selbox)
            {
                auto limits = panel - dot_11;
                auto curtop = std::clamp(seltop, dot_00, limits);
                auto curend = std::clamp(selend, dot_00, limits);
                auto grip_1 = rect{ curtop, dot_11 };
                auto grip_2 = rect{ curend, dot_11 };
                grip_1.coor += canvas.coor();
                grip_2.coor += canvas.coor();
                auto square = grip_1 | grip_2;
                square.normalize_itself();
                if (selbox || grip_1.coor.y == grip_2.coor.y)
                {
                    selmod == mime::disabled ||
                    selmod == mime::textonly ||
                    selmod == mime::safetext ? buffer.s11n<faux>(canvas, square)
                                             : buffer.s11n<true>(canvas, square);
                }
                else
                {
                    if (grip_1.coor.y > grip_2.coor.y) std::swap(grip_1, grip_2);
                    auto part_1 = rect{ grip_1.coor,             { panel.x - grip_1.coor.x, 1              }};
                    auto part_2 = rect{{ 0, grip_1.coor.y + 1 }, { panel.x, std::max(0, square.size.y - 2) }};
                    auto part_3 = rect{{ 0, grip_2.coor.y     }, { grip_2.coor.x + 1, 1                    }};
                    if (selmod == mime::textonly
                     || selmod == mime::safetext
                     || selmod == mime::disabled)
                    {
                        buffer.s11n<faux, true, faux>(canvas, part_1);
                        buffer.s11n<faux, faux, faux>(canvas, part_2);
                        buffer.s11n<faux, faux, true>(canvas, part_3);
                    }
                    else
                    {
                        buffer.s11n<true, true, faux>(canvas, part_1);
                        buffer.s11n<true, faux, faux>(canvas, part_2);
                        buffer.s11n<true, faux, true>(canvas, part_3);
                    }
                }
            }
            // bufferbase: Find the next match in the specified canvas and return true if found.
            auto selection_search(rich const& canvas, si32 from, feed direction, twod& seltop, twod& selend)
            {
                auto find = [&](auto a, auto b, auto& uinext, auto& uiprev)
                {
                    auto length = std::max(1, canvas.size().x);
                    auto offset = from + a;
                    if (canvas.find(match, offset, direction))
                    {
                        seltop = { offset % length,
                                   offset / length };
                        offset += a - b + 1;
                        selend = { offset % length,
                                   offset / length };
                        offset += a;
                        uinext = canvas.find(match, offset, direction); // Try to find next next.
                        uiprev = true;
                        return true;
                    }
                    else
                    {
                        uinext = faux;
                        return faux;
                    }
                };
                return direction == feed::fwd ? find(match.length(), 2, uifwd, uirev)
                                              : find(0, match.length(), uirev, uifwd);
            }
            // bufferbase: Return match navigation state.
    virtual si32 selection_button(twod /*delta*/ = {})
            {
                auto forward_is_available = uifwd ? 1 << 0 : 0;
                auto reverse_is_available = uirev ? 1 << 1 : 0;
                return forward_is_available | reverse_is_available;
            }
            // bufferbase: Clear scrollback keeping current line.
    virtual void clear_scrollback() = 0;
            // bufferbase: Proceed 2d text.
            template<class Span>
            void data_2d(twod block_size, Span const& proto, auto print_stripe)
            {
                assert(block_size.y > 1);
                char_2d.unpack2d(proto, block_size);
                auto clip = char_2d.clip();
                auto size = char_2d.size();
                auto wrapln = parser::style.wrp() == wrap::on;
                auto is_first = coord.x == 0 || (coord.x >= panel.x && wrapln);
                auto print_stripes = [&]
                {
                    auto head = char_2d.begin() + clip.coor.y * size.x;
                    auto tail = head + clip.size.y * size.x;
                    auto rest = size.x - (clip.coor.x + clip.size.x);
                    while (true)
                    {
                        head += clip.coor.x;
                        auto next = head + clip.size.x;
                        auto line = std::span(head, next);
                        print_stripe(line);
                        head = next + rest;
                        if (head != tail)
                        {
                            _cub(clip.size.x);
                            _lf(1);
                        }
                        else break;
                    }
                };

                if (!is_first) // Go up to make room for 2D char.
                {
                    _ri(block_size.y - 1);
                }
                if (wrapln)
                {
                    auto left = clip;
                    while (left.size.x > 0)
                    {
                        clip.coor.x = left.coor.x;
                        clip.size.x = std::min(panel.x - coord.x % panel.x, left.size.x);
                        print_stripes();
                        left.coor.x += clip.size.x;
                        left.size.x -= clip.size.x;
                        if (left.size.x > 0)
                        {
                            _cub(coord.x);
                            _lf(1);
                        }
                    }
                }
                else
                {
                    print_stripes();
                }
            }

            // bufferbase: Update terminal status.
            bool update_status(term_state& status) const
            {
                auto changed = faux;
                if (auto v = get_size(); status.size != v) { changed = true; status.size = v; }
                if (auto v = get_peak(); status.peak != v) { changed = true; status.peak = v; }
                if (auto v = get_mxsz(); status.mxsz != v) { changed = true; status.mxsz = v; }
                if (auto v = get_step(); status.step != v) { changed = true; status.step = v; }
                if (auto v = get_view(); status.area != v) { changed = true; status.area = v; }
                if (auto v = selection_active(); status.hash != v)
                {
                    changed = true;
                    status.hash = v;
                    if (status.hash)
                    {
                        status.mode = get_mode();
                        selection_status(status);
                    }
                }
                return changed;
            }
            void wrapdn() // Only for coord.x > panel.x.
            {
                coord.y += (coord.x + (panel.x - 1)) / panel.x - 1;
                coord.x  = (coord.x - 1) % panel.x + 1;
            }
            void wrapup() // Only for negative coord.x.
            {
                coord.y += (coord.x - (panel.x - 1)) / panel.x + 1 - 1;
                coord.x  = (coord.x + 1) % panel.x + panel.x - 1;
            }
        };
