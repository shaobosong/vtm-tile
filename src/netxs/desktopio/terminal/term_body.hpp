        using prot = input::keybd::prot;
        using buffer_ptr = bufferbase*;
        using vtty = os::vt::vtty;

        std::array<face, 5> pocket; // term: Buffers for DECCRA.
        termconfig defcfg; // term: Terminal settings.
        scroll_buf normal; // term: Normal    screen buffer.
        alt_screen altbuf; // term: Alternate screen buffer.
        buffer_ptr target; // term: Current   screen buffer pointer.
        pro::caret& caret; // term: Text cursor controller.
        pro::timer& timer; // term: Linear animation controller.
        pro::robot& robot; // term: Linear animation controller.
        m_tracking mtrack; // term: VT-style mouse tracking object.
        f_tracking ftrack; // term: Keyboard focus tracking object.
        w_tracking wtrack; // term: Terminal title tracking object.
        c_tracking ctrack; // term: Custom terminal palette tracking object.
        term_state status; // term: Screen buffer status info.
        cell       defclr; // term: Default/current colors (SGR49/39).
        twod       origin; // term: Viewport position.
        twod       follow; // term: Viewport follows cursor (bool: X, Y).
        bool       insmod; // term: Insert/replace mode.
        bool       decckm; // term: Cursor keys Application(true)/ANSI(faux) mode.
        bool       bpmode; // term: Bracketed paste mode.
        bool       unsync; // term: Viewport is out of sync.
        bool       invert; // term: Inverted rendering (DECSCNM).
        bool       styled; // term: Line style reporting.
        bool       io_log; // term: Stdio logging.
        bool       selalt; // term: Selection form (rectangular/linear).
        dragmode   seldrag; // term: Special drag selection mode.
        flag       resume; // term: Restart scheduled.
        flag       restart_pending; // term: Reset the session state before the next launch.
        flag       forced; // term: Forced shutdown.
        si32       selmod; // term: Selection mode.
        si32       onesht; // term: Selection one-shot mode.
        si32       altscr; // term: Alternate scroll mode.
        prot       kbmode; // term: Keyboard input mode.
        escx       w32key; // term: win32-input-mode forward buffer.
        bool       ime_on; // term: IME composition is active.
        para       imebox; // term: IME composition preview render.
        text       imetxt; // term: IME composition preview source.
        flow       imefmt; // term: IME composition preview layout.
        eccc       appcfg; // term: Application startup inits.
        hook       onerun; // term: One-shot token for restart session.
        bool       rawkbd; // term: Exclusive keyboard access.
        bool       bottom_anchored; // term: Anchor scrollback content when resizing (default is anchor at bottom).
        ui32       event_sources; // term: vt-input-mode event reporting bit-field.
        vtty       ipccon; // term: IPC connector. Should be destroyed first.

        // term: Place rectangle block to the scrollback buffer.
        template<class S, class P>
        auto write_block(S& scrollback, core const& block, twod coor, rect trim, P fuse)
        {
            auto size = block.size();
            auto clip = block.clip();
            auto dest = rect{ coor, clip.size };
            trim.trimby(dest);
            clip -= dest - trim;
            coor = trim.coor;
            auto head = block.begin() + clip.coor.y * size.x;
            auto tail = head + clip.size.y * size.x;
            auto rest = size.x - (clip.coor.x + clip.size.x);
            auto save = scrollback.coord;
            assert(rest >= 0);
            while (head != tail)
            {
                head += clip.coor.x;
                auto next = head + clip.size.x;
                auto line = std::span(head, next);
                scrollback.cup2(coor);
                scrollback.template _data<true>(clip.size.x, line, fuse);
                head = next + rest;
                coor.y++;
            }
            scrollback.cup2(save);
        }
        // term: CSI srcTop ; srcLeft ; srcBottom ; srcRight ; srcBuffIndex ; dstTop ; dstLeft ; dstBuffIndex $ v  — Copy rectangular area (DECCRA). BuffIndex: 1..6, 1 is default index. All coords are 1-based (inclusive).
        void deccra(fifo& q)
        {
            auto srcTop       = q(0);
            auto srcLeft      = q(0);
            auto srcBottom    = q(0);
            auto srcRight     = q(0);
            auto srcBuffIndex = std::clamp(q(0), 1, 6) - 2; // Pocket buffers are 2..6 (0..4). -1 it is a real buffer.
            auto dstTop       = q(0);
            auto dstLeft      = q(0);
            auto dstBuffIndex = std::clamp(q(0), 1, 6) - 2; // Pocket buffers are 2..6 (0..4). -1 it is a real buffer.
            auto fragment = face{};
            auto& console = *target;
            auto size = srcBuffIndex == -1 ? console.panel : pocket[srcBuffIndex].size();
            srcRight  += srcRight  ? 0 : size.x;
            srcBottom += srcBottom ? 0 : size.y;
            srcLeft   -= srcLeft   ? 1 : 0;
            srcTop    -= srcTop    ? 1 : 0;
            dstLeft   -= dstLeft   ? 1 : 0;
            dstTop    -= dstTop    ? 1 : 0;
            auto area = rect{{ srcLeft, srcTop }, { srcRight - srcLeft, srcBottom - srcTop }};
            auto src_area = rect{ dot_00, size };
            area.trimby(src_area);
            fragment.full(src_area);
            fragment.core::crop(area.size, defclr);
            fragment.core::area(area);
            // Take fragment.
            if (srcBuffIndex == -1) console.do_viewport_copy(fragment);
            else                    netxs::onbody(fragment, pocket[srcBuffIndex], cell::shaders::full);
            // Put fragment.
            auto coor = twod{ dstLeft, dstTop };
            if (dstBuffIndex == -1) // Dst is real buffer.
            {
                area.coor = {};
                fragment.area(area);
                if (target == &normal) write_block(normal, fragment, coor, src_area, cell::shaders::full);
                else
                {
                    auto& target_buffer = *(alt_screen*)target;
                    write_block(target_buffer, fragment, coor, src_area, cell::shaders::full);
                }
            }
            else
            {
                area.coor = coor;
                fragment.area(area);
                auto& dst = pocket[dstBuffIndex];
                dst.crop(console.panel, defclr);
                dst.plot(fragment, cell::shaders::full);
            }
        }
        // term: Terminal notification (OSC 9).
        void osc_notify(view data)
        {
            auto delimpos = data.find(';');
            if (delimpos != text::npos)
            {
                static constexpr auto cwdsync = "9"sv;
                auto type = data.substr(0, delimpos++);
                if (io_log) log("\tOSC %%;%type% notify: ", ansi::osc_term_notify, type, ansi::hi(utf::debase<faux, faux>(data.substr(delimpos))));
                if (type == cwdsync)
                {
                    auto path = text{ data.substr(delimpos) };
                    base::enqueue([&, path](auto& /*boss*/) mutable
                    {
                        base::riseup(tier::preview, e2::form::prop::cwd, path);
                    });
                }
            }
        }
        // term: Forward clipboard data (OSC 52).
        void forward_clipboard(view data)
        {
            auto clipdata = input::clipdata{};
            auto delimpos = data.find(';');
            if (delimpos != text::npos)
            {
                clipdata.meta = data.substr(0, delimpos++);
                clipdata.utf8.clear();
                utf::unbase64(data.substr(delimpos), clipdata.utf8);
                clipdata.form = mime::disabled;
                clipdata.size = target->panel;
                clipdata.hash = datetime::now();
                auto gates = base::riseup(tier::request, e2::form::state::keybd::enlist); // Take all foci.
                for (auto gate_id : gates) // Signal them to set the clipboard data.
                {
                    if (auto gear_ptr = base::getref<hids>(gate_id))
                    {
                        gear_ptr->set_clipboard(clipdata);
                    }
                }
            }
        }
        // term: Soft terminal reset (DECSTR).
        void decstr()
        {
            target->flush();
            normal.clear_all();
            altbuf.clear_all();
            target = &normal;
            invert = faux;
            decckm = faux;
            bpmode = faux;
            altscr = defcfg.def_alt_on;
            normal.brush.reset();
            ipccon.reset();
        }
        // term: Reset terminal attributes to defaults without touching UI preferences.
        void reset_attrs()
        {
            auto& console = *target;
            defclr.txt('\0').fgc(defcfg.def_fcolor).bgc(defcfg.def_bcolor).link(base::id);
            console.brush.reset(defclr);
            console.style.reset();
            console.style.wrp(defcfg.def_wrpmod);
            console.setpad(defcfg.def_margin);
            caret.style(defcfg.def_cursor);
        }
        // term: Reset runtime state before starting a new session.
        void reset_session()
        {
            decstr();
            insmod = faux;
            styled = faux;
            origin = {};
            follow = { 0, 1 };
            unsync = true;
            ime_on = faux;
            imetxt.clear();
            imebox.wipe();
            imefmt.flow::reset();
            w32key.clear();
            event_sources = {};
            onerun.reset();
            timer.pacify();
            robot.pacify();
            mtrack.reset();
            ftrack.set(faux);
            ctrack.reset();
            reset_attrs();
            caret.color(defcfg.def_curclr);
            caret.blink_period(defcfg.def_period);
            if (defcfg.def_cur_on) caret.show();
            else                   caret.hide();
            wtrack.reset();
            base::deface();
        }
        // term: Set termnail parameters. (DECSET).
        void _decset(si32 n)
        {
            switch (n)
            {
                case 1:    // Cursor keys application mode.
                    decckm = true;
                    break;
                case 3:    // Set 132 column window size (DECCOLM).
                    window_resize({ 132, 0 });
                    target->ed(commands::erase::display::viewport);
                    break;
                case 5:    // Inverted rendering (DECSCNM).
                    invert = true;
                    break;
                case 6:    // Enable origin mode (DECOM).
                    target->decom = true;
                    target->cup0(dot_00);
                    break;
                case 7:    // Enable auto-wrap.
                    target->style.wrp(wrap::on);
                    break;
                case 12:   // Enable cursor blinking.
                    caret.blink_period();
                    break;
                case 25:   // Cursor on.
                    caret.show();
                    break;
                case 9:    // Enable X10 mouse reporting protocol.
                    log(prompt::term, "CSI ? 9 h  X10 Mouse reporting protocol is not supported");
                    break;
                case 1000: // Enable mouse buttons reporting mode.
                    mtrack.enable(input::mouse::mode::buttons_press);
                    break;
                case 1001: // Use Hilite mouse tracking mode.
                    log(prompt::term, "CSI ? 1001 h  Hilite mouse tracking mode is not supported");
                    break;
                case 1002: // Enable mouse buttons and drags reporting mode.
                    mtrack.enable(input::mouse::mode::buttons_drags);
                    break;
                case 1003: // Enable all mouse events reporting mode.
                    mtrack.enable(input::mouse::mode::all_movements);
                    break;
                case 1004: // Enable focus tracking.
                    ftrack.set(true);
                    break;
                case 1005: // Enable UTF8 mousee position encoding.
                    mtrack.enable(input::mouse::mode::utf8);
                    break;
                case 1006: // Enable SGR mouse reporting protocol.
                    mtrack.setmode(input::mouse::prot::sgr);
                    break;
                case 1007: // Enable alternate scroll mode.
                    altscr = defcfg.def_alt_on;
                    break;
                case 10060:// Enable mouse reporting outside the viewport (outside+negative coordinates).
                    mtrack.enable(input::mouse::mode::negative_args);
                    break;
                case 1015: // Enable URXVT mouse reporting protocol.
                    log(prompt::term, "CSI ? 1015 h  URXVT mouse reporting protocol is not supported");
                    break;
                case 1016: // Enable Pixels (subcell) mouse mode.
                    log(prompt::term, "CSI ? 1016 h  Pixels (subcell) mouse mode is not supported");
                    break;
                case 1048: // Save cursor pos.
                    target->scp();
                    break;
                case 1047: // Use alternate screen buffer.
                case 1049: // Save cursor pos and use alternate screen buffer, clearing it first.  This control combines the effects of the 1047 and 1048  modes.
                    if (target != &normal && target != &altbuf) break; // Suppress mode change for additional screen buffers (windows console).
                    altbuf.style = target->style; // Inherit the normal buffer brush.
                    altbuf.brush = target->brush; //
                    altbuf.clear_all();
                    altbuf.resize_viewport(target->panel); // Reset viewport to the basis.
                    target = &altbuf;
                    follow[axis::Y] = true;
                    break;
                case 2004: // Set bracketed paste mode.
                    bpmode = true;
                    break;
                default:
                    break;
            }
        }
        // term: Set termnail parameters. (DECSET).
        void decset(si32 n)
        {
            target->flush();
            _decset(n);
        }
        // term: Set termnail parameters. (DECSET).
        void decset(fifo& q)
        {
            target->flush();
            while (auto next = q(0)) _decset(next);
        }
        // term: Switch buffer to normal and reset viewport to the basis.
        template<class T>
        void reset_to_normal(T& a)
        {
            normal.resize_viewport(a.panel);
            target = &normal;
            follow[axis::Y] = true;
        }
        // term: Switch buffer to altbuf.
        template<class T>
        void reset_to_altbuf(T& altbuf)
        {
            altbuf.resize_viewport(target->panel);
            target = &altbuf;
        }
        // term: Reset terminal parameters. (DECRST).
        void _decrst(si32 n)
        {
            switch (n)
            {
                case 1:    // Cursor keys ANSI mode.
                    decckm = faux;
                    break;
                case 3:    // Set 80 column window size (DECCOLM).
                    window_resize({ 80, 0 });
                    target->ed(commands::erase::display::viewport);
                    break;
                case 5:    // Inverted rendering (DECSCNM).
                    invert = faux;
                    break;
                case 6:    // Disable origin mode (DECOM).
                    target->decom = faux;
                    target->cup0(dot_00);
                    break;
                case 7:    // Disable auto-wrap.
                    target->style.wrp(wrap::off);
                    break;
                case 12:   // Disable cursor blinking.
                    caret.blink_period(span::zero());
                    break;
                case 25:   // Cursor off.
                    caret.hide();
                    break;
                case 9:    // Disable X10 mouse reporting protocol.
                    log(prompt::term, "CSI ? 9 l  X10 Mouse tracking protocol is not supported");
                    break;
                case 1000: // Disable mouse buttons reporting mode.
                    mtrack.disable(input::mouse::mode::buttons_press);
                    break;
                case 1001: // Don't use Hilite(c) mouse tracking mode.
                    log(prompt::term, "CSI ? 1001 l  Hilite mouse tracking mode is not supported");
                    break;
                case 1002: // Disable mouse buttons and drags reporting mode.
                    mtrack.disable(input::mouse::mode::buttons_drags);
                    break;
                case 1003: // Disable all mouse events reporting mode.
                    mtrack.disable(input::mouse::mode::all_movements);
                    break;
                case 1004: // Disable focus tracking.
                    ftrack.set(faux);
                    break;
                case 1005: // Disable UTF-8 mouse reporting protocol.
                    mtrack.disable(input::mouse::mode::utf8);
                    break;
                case 1006: // Disable SGR mouse reporting protocol (set X11 mode).
                    mtrack.setmode(input::mouse::prot::x11);
                    mtrack.disable(input::mouse::mode::all_movements);
                    break;
                case 1007: // Disable alternate scroll mode.
                    altscr = 0;
                    break;
                case 10060:// Disable mouse reporting outside the viewport (allow reporting inside the viewport only).
                    mtrack.disable(input::mouse::mode::negative_args);
                    break;
                case 1015: // Disable URXVT mouse reporting protocol.
                    log(prompt::term, "CSI ? 1015 l  URXVT mouse reporting protocol is not supported");
                    break;
                case 1016: // Disable Pixels (subcell) mouse mode.
                    log(prompt::term, "CSI ? 1016 l  Pixels (subcell) mouse mode is not supported");
                    break;
                case 1048: // Restore cursor pos.
                    target->rcp();
                    break;
                case 1047: // Use normal screen buffer.
                case 1049: // Use normal screen buffer and restore cursor. Use the old normal buffer brush.
                    if (target != &normal && target != &altbuf) break; // Suppress mode change for additional screen buffers (windows console).
                    reset_to_normal(*target);
                    break;
                case 2004: // Disable bracketed paste mode.
                    bpmode = faux;
                    break;
                default:
                    break;
            }
        }
        // term: Reset termnail parameters. (DECRST).
        void decrst(si32 n)
        {
            target->flush();
            _decrst(n);
        }
        // term: Reset termnail parameters. (DECRST).
        void decrst(fifo& q)
        {
            target->flush();
            while (auto next = q(0)) _decrst(next);
        }
        // term: Set termnail parameters.
        void _modset(si32 n)
        {
            switch (n)
            {
                case 4:     // Insert/Replace Mode (IRM) on.
                    insmod = true;
                    break;
                case 20:    // LNM-Line Feed/New Line Mode on.
                    target->set_autocr(true);
                    break;
                default:
                    break;
            }
        }
        // term: Reset termnail parameters.
        void _modrst(si32 n)
        {
            switch (n)
            {
                case 4:     // Insert/Replace Mode (IRM) off.
                    insmod = faux;
                    break;
                case 20:    // LNM-Line Feed/New Line Mode off.
                    target->set_autocr(faux);
                    break;
                default:
                    break;
            }
        }
        // term: Set termnail parameters.
        void modset(fifo& q)
        {
            target->flush();
            while (auto next = q(0)) _modset(next);
        }
        // term: Reset termnail parameters.
        void modrst(fifo& q)
        {
            target->flush();
            while (auto next = q(0)) _modrst(next);
        }
        // term: Set scrollback buffer size and grow step.
        void sbsize(fifo& q)
        {
            target->flush();
            auto ring_size = q.subarg(defcfg.def_length);
            auto grow_step = q.subarg(defcfg.def_growdt);
            auto grow_mxsz = q.subarg(defcfg.def_growmx);
            normal.resize_history(ring_size, grow_step, grow_mxsz);
        }
        // term: Check and update scrollback buffer limits.
        void sb_min(si32 min_length)
        {
            target->flush();
            if (normal.batch.step == 0 && normal.batch.peak < min_length)
            {
                normal.resize_history(min_length);
            }
        }
        // term: Write input data.
        void write(view data)
        {
            ipccon.write(data);
        }
        // term: Write tty data and flush the queue.
        void answer(escx& queue)
        {
            if (queue.length())
            {
                write(queue);
                queue.clear();
            }
        }
        // term: Reset viewport position.
        void scroll(twod& origin_ref)
        {
            auto& console = *target;
            if (follow[axis::Y])
            {
                if (follow[axis::X])
                {
                    follow[axis::X] = faux;
                    auto pos = console.get_coord(dot_00).x;
                    origin_ref.x = bufferbase::reset_viewport(origin_ref.x, pos, console.panel.x);
                }
            }
            origin_ref.y = console.get_origin(follow[axis::Y]);
        }
        // term: Proceed terminal changes.
        template<class P>
        void update(P proc)
        {
            auto lock = bell::sync();
            if (defcfg.resetonout) follow[axis::Y] = true;
            if (follow[axis::Y])
            {
                unsync |= proc();
            }
            else
            {
                auto last_basis = target->get_basis();
                auto last_slide = target->get_slide();
                auto is_changed = proc();
                unsync |= is_changed;
                if (is_changed)
                {
                    auto next_basis = target->get_basis();
                    follow[axis::Y] = (last_basis <= last_slide && last_slide <= next_basis)
                                   || (next_basis <= last_slide && last_slide <= last_basis);
                }
            }
        }
        // term: Proceed terminal input.
        template<bool Forced = faux>
        auto ondata_direct(view data = {}, bufferbase* target_buffer = {})
        {
            auto& console_ptr = target_buffer ? target_buffer : target;
            if (data.size())
            {
                if (io_log) log(prompt::cout, "\n\t", utf::replace_all(ansi::hi(utf::debase(data)), "\n", ansi::pushsgr().nil().add("\n\t").popsgr()));
                ansi::parse(data, console_ptr);
                return true;
            }
            else
            {
                console_ptr->parser::flush(); // Update line style, etc.
                return Forced;
            }
        }
        // term: Proceed terminal input.
        template<bool Forced = faux>
        void ondata(view data = {}, bufferbase* target_buffer = {})
        {
            update([&]
            {
                return ondata_direct<Forced>(data, target_buffer);
            });
        }
        // term: Reset to defaults.
        void setdef()
        {
            reset_attrs();
            selection_selmod(defcfg.def_selmod);
        }
        // term: Set terminal background.
        void setsgr(fifo& q)
        {
            struct marker
            {
                ansi::deco style;
                ansi::mark brush;
                void task(ansi::rule const& /*cmd*/) { }
            };
            static auto parser = ansi::csi_t<marker, true>{};

            auto mark = marker{};
            mark.brush.reset(defclr);
            auto ptr = &mark;
            parser.dispatch_sgr(q, ptr, true);
            set_color(mark.brush);
        }
        // term: CCC_LSR: Enable line style reporting.
        void setlsr(bool state)
        {
            styled = state;
            if (styled) ipccon.style(target->parser::style, kbmode);
        }
        // term: Request to scroll inside viewport and return actual delta.
        auto scrollby(twod delta)
        {
            auto coor = base::coor();
            auto info = e2::form::upon::scroll::bystep::v.param({ .vector = delta });
            base::riseup(tier::preview, e2::form::upon::scroll::bystep::v, info);
            return base::coor() - coor;
        }
        // term: Is the selection allowed.
        auto selection_passed()
        {
            return selmod != mime::disabled;
        }
        // term: Set selection mode.
        void selection_selmod(si32 newmod)
        {
            selmod = newmod;
            base::signal(tier::release, e2::form::draggable::left, selection_passed());
            base::signal(tier::release, terminal::events::selmod, selmod);
        }
        // term: Run one-shot selection.
        void selection_oneshot(si32 newmod)
        {
            onesht = newmod;
            base::signal(tier::release, terminal::events::onesht, onesht);
            selection_selmod(newmod);
        }
        // term: Set selection form.
        void selection_selalt(bool boxed)
        {
            selalt = boxed;
            base::signal(tier::release, e2::form::draggable::left, selection_passed());
            base::signal(tier::release, terminal::events::selalt, selalt);
        }
        // term: Set the next selection mode.
        void selection_selmod()
        {
            auto newmod = (selmod + 1) % mime::count;
            selection_selmod(newmod);
        }
        // term: Toggle selection form.
        void selection_toggle_selalt()
        {
            selection_selalt(!selalt);
            target->selection_locked(faux);
            target->selection_selbox(selalt);
            target->selection_update();
            base::deface();
        }
        void selection_drag_cancel()
        {
            target->selection_drag_clear();
            seldrag = dragmode::none;
        }
        auto selection_cancel()
        {
            selection_drag_cancel();
            auto active = target->selection_cancel();
            if (active)
            {
                auto& console = *target;
                timer.pacify();
                auto shore = console.getpad();
                auto delta = dot_00;
                     if (origin.x <= oversz.l && origin.x > oversz.l - shore) delta = {-1, oversz.l - shore };
                else if (origin.x >=-oversz.r && origin.x < shore - oversz.r) delta = { 1, shore - oversz.r };
                if (delta.x)
                {
                    auto limit = delta.y;
                    delta.y = 0;
                    timer.actify(commands::ui::center, 0ms, [&, delta, shore, limit](auto /*id*/) mutable // 0ms = current FPS ticks/sec.
                    {
                        auto shift = scrollby(delta);
                        return shore-- && (origin.x != limit && !!shift);
                    });
                }
                base::deface();
            }
            return active;
        }
        auto get_clipboard_text(hids& gear)
        {
            gear.owner.base::signal(tier::request, input::events::clipboard, gear);
            auto& data = gear.board::cargo;
            if (data.utf8.size())
            {
                if (data.form == mime::richtext)
                {
                    auto post = page{ data.utf8 };
                    return post.to_rich();
                }
                else if (data.form == mime::htmltext)
                {
                    auto post = page{ data.utf8 };
                    auto [html, code] = post.to_html();
                    return code;
                }
            }
            return data.utf8;
        }
        auto _paste(auto& data)
        {
            //todo pasting must be ready to be interruped by any pressed key (to interrupt a huge paste).
            follow[axis::X] = true;
            follow[axis::Y] = true;
            ipccon.paste(data, bpmode, kbmode);
        }
        auto paste(hids& gear)
        {
            auto data = get_clipboard_text(gear);
            if (data.size())
            {
                pro::focus::set(This(), gear.id, solo::off);
                _paste(data);
                return true;
            }
            return faux;
        }
        auto _copy(hids& gear, text const& data)
        {
            auto form = selmod == mime::disabled ? mime::textonly : selmod;
            pro::focus::set(This(), gear.id, solo::off);
            gear.set_clipboard(target->panel, data, form);
        }
        auto copy(hids& gear)
        {
            auto data = target->selection_pickup(selmod);
            if (data.size())
            {
                _copy(gear, data);
            }
            auto ctrl_pressed = gear.meta(hids::anyCtrl);
            if (onesht != mime::disabled && !ctrl_pressed)
            {
                selection_oneshot(mime::disabled);
            }
            if (ctrl_pressed || selection_cancel()) // Keep selection if Ctrl is pressed.
            {
                return true;
            }
            return faux;
        }
        auto prnscrn(hids& gear)
        {
            auto selbox = true;
            auto square = target->panel;
            auto seltop = dot_00;
            auto selend = square - dot_11;
            auto buffer = escx{};
            auto canvas = e2::render::any.param();
            canvas.size(square);
            canvas.full({ origin, square });
            base::signal(tier::release, e2::render::any, canvas);
            target->bufferbase::selection_pickup(buffer, canvas, seltop, selend, selmod, selbox);
            if (buffer.size()) buffer.eol();
            _copy(gear, buffer);
        }
        auto selection_active()
        {
            return target->selection_active();
        }
        void selection_pickup(hids& gear)
        {
            auto gear_test = base::riseup(tier::request, e2::form::state::keybd::find, { gear.id, 0 });
            if (!gear_test.second) // Set exclusive focus on right click.
            {
                pro::focus::set(This(), gear.id, solo::on);
            }
            if ((selection_active() && copy(gear))
             || (selection_passed() && paste(gear)))
            {
                gear.dismiss();
            }
        }
        void selection_mclick(hids& gear)
        {
            auto& console = *target;
            auto utf8 = text{};
            if (console.selection_active()) // Paste from selection.
            {
                utf8 = console.match.utf8();
            }
            else if (selection_passed()) // Paste from clipboard.
            {
                utf8 = get_clipboard_text(gear);
            }
            if (utf8.size())
            {
                pro::focus::set(This(), gear.id, solo::off);
                follow[axis::X] = true;
                if (bpmode)
                {
                    auto temp = text{};
                    temp.reserve(utf8.size() + ansi::paste_begin.size() + ansi::paste_end.size());
                    temp += ansi::paste_begin;
                    temp += utf8;
                    temp += ansi::paste_end;
                    std::swap(utf8, temp);
                }
                data_out(utf8);
                gear.dismiss();
            }
        }
        void selection_lclick(hids& gear)
        {
            auto& console = *target;
            auto go_on = gear.meta(hids::anyCtrl);
            if (go_on && console.selection_active())
            {
                console.selection_follow(gear.coord, go_on);
                selection_extend(gear);
                gear.dismiss();
            }
            else selection_cancel();
        }
        void selection_dblpress(hids& gear)
        {
            seldrag = dragmode::word;
            target->selection_drag_word_start(gear.coord);
            gear.dismiss();
            base::deface();
        }
        void selection_tplpress(hids& gear)
        {
            if (gear.clicked != 3) return;
            seldrag = dragmode::line;
            target->selection_drag_line_start(gear.coord);
            gear.dismiss();
            base::deface();
        }
        void selection_dblclk(hids& gear)
        {
            selection_drag_cancel();
            target->selection_byword(gear.coord);
            gear.dismiss();
            base::deface();
        }
        void selection_tplclk(hids& gear)
        {
            selection_drag_cancel();
            if (gear.clicked == 3) target->selection_byline(gear.coord);
            else if (gear.clicked >= 4) target->selection_selall();
            gear.dismiss();
            base::deface();
        }
        void selection_worddrag(twod coord)
        {
            auto limits = rect{ -origin, target->panel };
            for (auto a : { axis::X, axis::Y })
            {
                     if (coord[a] <  limits.coor[a])                 coord[a] = limits.coor[a];
                else if (coord[a] >= limits.coor[a] + limits.size[a]) coord[a] = limits.coor[a] + limits.size[a] - 1;
            }
            target->selection_drag_word_pull(coord);
            base::deface();
        }
        void selection_linedrag(twod coord)
        {
            auto limits = rect{ -origin, target->panel };
            for (auto a : { axis::X, axis::Y })
            {
                     if (coord[a] <  limits.coor[a])                 coord[a] = limits.coor[a];
                else if (coord[a] >= limits.coor[a] + limits.size[a]) coord[a] = limits.coor[a] + limits.size[a] - 1;
            }
            target->selection_drag_line_pull(coord);
            base::deface();
        }
        void selection_create(hids& gear)
        {
            if (seldrag != dragmode::none)
            {
                base::deface();
                return;
            }
            auto& console = *target;
            auto boxed = selalt ^ !!gear.meta(hids::anyAlt);
            auto go_on = gear.meta(hids::anyCtrl);
            console.selection_follow(gear.click, go_on);
            if (go_on) console.selection_extend(gear.click, boxed);
            else       console.selection_create(gear.click, boxed);
            base::deface();
        }
        void selection_moveto(twod delta)
        {
            if (delta)
            {
                auto path = delta;
                auto time = datetime::round<si32>(skin::globals().switching);
                auto init = 0;
                auto func = constlinearAtoB<twod>(path, time, init);
                robot.actify(func, [&](twod& step)
                {
                    scrollby(step);
                    base::deface();
                });
            }
            else timer.pacify();

        }
        void selection_extend(hids& gear)
        {
            // Check bounds and scroll if needed.
            auto& console = *target;
            auto specialdrag = seldrag;
            auto boxed = selalt ^ !!gear.meta(hids::anyAlt);
            auto coord = twod{ gear.coord };
            auto vport = rect{ -origin, console.panel };
            auto delta = dot_00;
            for (auto a : { axis::X, axis::Y })
            {
                     if (coord[a] <  vport.coor[a])                 delta[a] = vport.coor[a] - coord[a];
                else if (coord[a] >= vport.coor[a] + vport.size[a]) delta[a] = vport.coor[a] + vport.size[a] - coord[a] - 1;
            }
            if (delta)
            {
                auto shift = scrollby(delta);
                coord += delta - shift;
                delta -= delta * 3 / 4; // Decrease scrolling speed.
                timer.actify(0ms, [&, delta, coord, boxed, specialdrag](auto) mutable // 0ms = current FPS ticks/sec.
                                    {
                                        auto shift = scrollby(delta);
                                        coord -= shift;
                                        if (specialdrag == dragmode::word)
                                        {
                                            selection_worddrag(coord);
                                            return !!shift;
                                        }
                                        else if (specialdrag == dragmode::line)
                                        {
                                            selection_linedrag(coord);
                                            return !!shift;
                                        }
                                        else if (console.selection_extend(coord, boxed))
                                        {
                                            base::deface();
                                            return !!shift;
                                        }
                                        else return faux;
                                    });
            }
            else timer.pacify();

            if (specialdrag == dragmode::word)
            {
                selection_worddrag(coord);
            }
            else if (specialdrag == dragmode::line)
            {
                selection_linedrag(coord);
            }
            else if (console.selection_extend(coord, boxed))
            {
                base::deface();
            }
        }
        void selection_finish(hids& /*gear*/)
        {
            //todo option: copy on select
            //...
            selection_drag_cancel();
            timer.pacify();
            base::deface();
        }
        void selection_submit()
        {
            LISTEN(tier::release, e2::form::drag::start ::left, gear){ if (selection_passed()) selection_create(gear); };
            LISTEN(tier::release, e2::form::drag::pull  ::left, gear){ if (selection_passed()) selection_extend(gear); };
            LISTEN(tier::release, e2::form::drag::stop  ::left, gear){                         selection_finish(gear); };
            LISTEN(tier::release, e2::form::drag::cancel::left, gear){                         selection_cancel();     };
            on(tier::mouserelease, input::key::RightClick,                [&](hids& gear){                         selection_pickup(gear); });
            on(tier::mouserelease, input::key::LeftClick,                 [&](hids& gear){                         selection_lclick(gear); });
            on(tier::mouserelease, input::key::MiddleClick,               [&](hids& gear){                         selection_mclick(gear); });
            on(tier::mouserelease, input::key::LeftDoublePress,           [&](hids& gear){ if (selection_passed()) selection_dblpress(gear); });
            on(tier::mouserelease, input::key::LeftDoubleClick,           [&](hids& gear){ if (selection_passed()) selection_dblclk(gear); });
            on(tier::mouserelease, input::key::LeftMultiPress,            [&](hids& gear){ if (selection_passed()) selection_tplpress(gear); });
            on(tier::mouserelease, input::key::LeftMultiClick,            [&](hids& gear){ if (selection_passed()) selection_tplclk(gear); });
            on(tier::mouserelease, input::key::MouseWheel, [&](hids& gear)
            {
                if (gear.captured()) // Forward mouse wheel events to all parents. Wheeling while button pressed.
                {
                    auto& offset = base::coor();
                    if (auto parent_ptr = base::parent())
                    {
                        auto& parent = *parent_ptr;
                        gear.pass(tier::mouserelease, parent, offset);
                    }
                }
                else
                {
                    if (gear.meta(hids::anyCtrl)) return; // Ctrl+Wheel is reserved for zooming.
                    if (altscr && target != &normal)
                    {
                        if (gear.whlsi)
                        {
                            auto count = std::abs(gear.whlsi);
                            auto arrow = decckm ? gear.whlsi > 0 ? "\033OA"sv : "\033OB"sv
                                                : gear.whlsi > 0 ? "\033[A"sv : "\033[B"sv;
                            data_out(utf::repeat(arrow, count));
                        }
                        gear.dismiss();
                    }
                }
            });
        }
        void selection_search(hids& gear, feed dir)
        {
            auto& console = *target;
            auto delta = dot_00;
            auto fwd = dir == feed::fwd;
            if (console.selection_active())
            {
                if (console.match.empty())
                {
                    delta.y = fwd ? -console.arena // Page by page scrolling if nothing to search.
                                  :  console.arena;
                }
                else delta = console.selection_search(dir);
            }
            else
            {
                gear.owner.base::signal(tier::request, input::events::clipboard, gear);
                auto& data = gear.board::cargo;
                if (data.utf8.size())
                {
                    delta = console.selection_search(dir, data.utf8);
                }
                else // Page by page scrolling if nothing to search.
                {
                    delta.y = fwd ? -console.arena
                                  :  console.arena;
                }
            }
            base::signal(tier::release, terminal::events::search::status, console.selection_button(delta));
            if (target == &normal && delta)
            {
                selection_moveto(delta);
            }
        }
        auto& get_color()
        {
            return defclr;
        }
        void set_color(cell brush)
        {
            auto& console = *target;
            brush.link(base::id);
            defclr = brush;
            brush.link(console.brush.link());
            console.brush.reset(brush);
        }
        void set_bg_color(argb bg)
        {
            auto& console = *target;
            auto brush = defclr;
            brush.bgc(bg);
            brush.link(base::id);
            defclr = brush;
            brush.link(console.brush.link());
            console.brush.reset(brush);
            base::signal(tier::release, terminal::events::colors::bg, bg);
        }
        void set_fg_color(argb fg)
        {
            auto& console = *target;
            auto brush = defclr;
            brush.fgc(fg);
            brush.link(base::id);
            defclr = brush;
            brush.link(console.brush.link());
            console.brush.reset(brush);
            base::signal(tier::release, terminal::events::colors::fg, fg);
        }
        void set_rawkbd(si32 state = {})
        {
            if (!state) rawkbd = !rawkbd;
            else        rawkbd = state - 1;
            base::signal(tier::release, terminal::events::rawkbd, rawkbd);
        }
        void set_wrapln(si32 wrapln = {})
        {
            target->selection_setwrp((wrap)wrapln);
            if (!target->selection_active())
            {
                follow[axis::Y] = true; // Reset viewport.
            }
            ondata<true>(); // Trigger to rise event.
        }
        void set_align(si32 align = {})
        {
            target->selection_setjet((bias)align);
            if (!target->selection_active())
            {
                follow[axis::Y] = true; // Reset viewport.
            }
            ondata<true>(); // Trigger to rise event.
        }
        void set_selmod(si32 mode)
        {
            selection_selmod(mode);
        }
        void set_oneshot(si32 mode)
        {
            selection_oneshot(mode);
        }
        void set_selalt(bool boxed)
        {
            selection_selalt(boxed);
        }
        void set_log(bool state)
        {
            if (defcfg.allow_logs)
            {
                io_log = state;
                base::signal(tier::release, terminal::events::io_log, state);
            }
        }
        void clear_scrollback()
        {
            target->clear_scrollback();
            ondata<true>();
        }
        void exec_cmd(commands::ui::commands cmd)
        {
            if constexpr (debugmode) log(prompt::term, "Command: ", cmd);
            auto& console = *target;
            switch (cmd)
            {
                case commands::ui::toggleraw:    set_rawkbd();                        break;
                case commands::ui::togglewrp:    console.selection_setwrp();          break;
                case commands::ui::togglejet:    console.selection_setjet();          break;
                case commands::ui::togglesel:    selection_selmod();                  break;
                case commands::ui::toggleselalt: selection_toggle_selalt();           break;
                case commands::ui::restart:      restart();                           break;
                case commands::ui::sighup:       close(true);                         break;
                case commands::ui::undo:         ipccon.undo(true);                   break;
                case commands::ui::redo:         ipccon.undo(faux);                   break;
                case commands::ui::deselect:     selection_cancel();                  break;
                default: break;
            }
            if (cmd != commands::ui::togglesel && !console.selection_active())
            {
                follow[axis::Y] = true; // Reset viewport.
            }
            ondata<true>();
        }
        void data_in(view data)
        {
            follow[axis::Y] = true;
            ondata(data);
        }
        void data_out(view data)
        {
            follow[axis::Y] = true;
            write(data);
        }
        void onexit(si32 code, text msg = {}, bool exit_after_sighup = faux)
        {
            if (exit_after_sighup)
            {
                close();
            }
            else base::enqueue<faux>([&, code, msg, backup = This()](auto& /*boss*/) mutable
            {
                ipccon.payoff(io_log);
                auto lock = bell::sync();
                auto error = [&]
                {
                    auto byemsg = escx{};
                    if (target != &normal) byemsg.locate({ 0, target->panel.y - 1 });
                    byemsg.bgc(code ? argb{ reddk } : argb{}).fgc(whitelt).add(msg)
                          .add("\r\nProcess exited with code ", os::exitcode(code)).nil()
                          .add("\r\n\n");
                    return byemsg;
                };
                auto query = [&]
                {
                    auto byemsg = error().add("Press Esc to close or press Enter to restart the session.\r\n")
                                         .add("\n");
                    ondata(byemsg);
                    LISTEN(tier::release, input::events::keybd::post, gear, onerun)
                    {
                        if (gear.keystat)
                        {
                            switch (gear.keybd::generic())
                            {
                                //todo key
                                case key::Esc:      close(); onerun.reset(); break;
                                case key::KeyEnter: restart_pending.exchange(true);
                                                    start();
                                                    onerun.reset(); break;
                            }
                        }
                    };
                    base::riseup(tier::release, e2::form::global::sysstart, 0);
                };
                auto renew = [&]
                {
                    auto byemsg = error().add("\n");
                    ondata(byemsg);
                    restart_pending.exchange(true);
                    start();
                };
                     if (forced)                close();
                else if (resume.exchange(faux)) renew();
                else switch (defcfg.def_atexit)
                {
                    case commands::atexit::smart: code ? query()
                                                       : close(); break;
                    case commands::atexit::retry: code ? renew()
                                                       : close(); break;
                    case commands::atexit::ask:          query(); break;
                    case commands::atexit::close:        close(); break;
                    case commands::atexit::restart:      renew(); break;
                }
                backup.reset(); // Backup should dtored under the lock.
            });
        }
        void start()
        {
            base::riseup(tier::release, e2::form::upon::started, This());
        }
        void start_term(eccc cfg, os::fdrw fds = {})
        {
            appcfg = cfg;
            if (!ipccon)
            {
                if (restart_pending.exchange(faux))
                {
                    reset_session();
                }
                base::enqueue([&, backup = This()](auto& /*boss*/) mutable // We can't request the title before conio.run(), so we queue the request.
                {
                    auto& title = wtrack.get(ansi::osc_title);
                    if (title.empty())
                    {
                        wtrack.set(ansi::osc_title); // Set default title if it is empty.
                    }
                    if (defcfg.send_input.size())
                    {
                        ipccon.write<faux>(defcfg.send_input);
                    }
                    backup.reset(); // Backup should dtored under the lock.
                });
                appcfg.win = target->panel;
                ipccon.runapp(*this, appcfg, fds);
            }
        }
        void restart()
        {
            resume.exchange(true);
            restart_pending.exchange(true);
            if (!ipccon.sighup(faux) && !ipccon.stdwrite.joinable())
            {
                resume.exchange(faux);
                start();
            }
        }
        void close(bool fast = true, bool notify = true)
        {
            if (notify)
            {
                base::signal(tier::request, e2::form::proceed::quit::one, fast);
            }
            forced = fast;
            if (ipccon)
            {
                if (ipccon.sighup())
                {
                    base::enqueue<faux>([&, backup = This()](auto& /*boss*/) mutable // This backup is to keep the task active.
                    {
                        ipccon.payoff(io_log); // Wait child process.
                        auto lock = bell::sync();
                        base::riseup(tier::release, e2::form::proceed::quit::one, forced);
                        backup.reset(); // Backup should dtored under the lock.
                    });
                }
            }
            else // Child process exited with non-zero code and term waits keypress.
            {
                auto lock = bell::sync();
                onerun.reset();
                base::enqueue([&, backup = This()](auto& /*boss*/) mutable // The termlink trailer (calling ui::term::close()) should be joined before ui::term dtor.
                {
                    base::riseup(tier::release, e2::form::proceed::quit::one, forced);
                    backup.reset(); // Backup should dtored under the lock.
                });
            }
        }
        // term: Resize terminal window.
        void window_resize(twod winsz)
        {
            base::riseup(tier::preview, e2::form::prop::window::size, winsz);
        }
        // term: Custom data output (ConSrv cooked read callback).
        template<class Fx>
        void data(rich& cooked, Fx fx)
        {
            if (auto width = cooked.length())
            {
                auto& proto = cooked.pick();
                auto& brush = target->parser::brush;
                cooked.each([&](cell& c){ c.meta(brush); });
                //todo split by char height and do _data2d(...) for each
                if (target == &normal) normal._data(width, proto, fx);
                else
                {
                    auto& target_buffer = *(alt_screen*)target;
                    target_buffer._data(width, proto, fx);
                }
            }
        }
        // term: Move composition cursor (imebox.caret) inside viewport with wordwrapping.
        auto calc_composition_cursor(twod viewport_square_size, twod viewport_cursor)
        {
            auto composit_cursor = viewport_cursor;
            auto step = 0;
            auto next = 0;
            auto test = [&](auto const& coord, auto const& subblock, auto /*isr_to_l*/)
            {
                next = step + subblock.length();
                if (imebox.caret >= step && imebox.caret <= next) // It could be evaluated twice if the cursor is wrapped.
                {
                    composit_cursor = coord;
                    composit_cursor.x += imebox.caret - step;
                }
                step = next;
            };
            imefmt.flow::reset();
            imefmt.flow::size({ viewport_square_size.x, viewport_square_size.y * 2 });
            imefmt.flow::ac(viewport_cursor);
            imefmt.flow::compose<faux>(imebox, test);
            return composit_cursor;
        }
        void key_event(hids& gear, bool forced_event = faux)
        {
            if (!forced_event && gear.touched && !rawkbd) return;
            switch (gear.payload)
            {
                case keybd::type::keypress:
                    if (gear.doinput()) selection_cancel();
                    if (defcfg.resetonkey && gear.doinput())
                    {
                        base::riseup(tier::release, e2::form::animate::reset, 0); // Reset scroll animation.
                        unsync = true;
                        follow[axis::X] = true;
                        follow[axis::Y] = true;
                    }
                    ipccon.keybd(gear, decckm, kbmode);
                    if (forced_event || !gear.touched || gear.keystat != input::key::released || rawkbd) gear.set_handled(faux);
                    break;
                case keybd::type::imeinput:
                case keybd::type::keypaste:
                    selection_cancel();
                    _paste(gear.cluster);
                    gear.dismiss();
                    break;
                case keybd::type::imeanons:
                    if (imetxt != gear.cluster)
                    {
                        imetxt = gear.cluster;
                        imebox.wipe();
                        ansi::parse(gear.cluster, &imebox);
                        ime_on = imebox.length();
                        if (ime_on)
                        {
                            imebox.style.wrp(wrap::on);
                            //if (imebox.locus.size())
                            auto iter = std::find_if(imebox.locus.begin(), imebox.locus.end(), [](auto cmd){ return cmd.cmd == ansi::fn::sc; });
                            if (iter != imebox.locus.end())
                            {
                                //auto [cmd, arg] = imebox.locus.back();
                                auto [cmd, arg] = *iter;
                                if (cmd == ansi::fn::sc) imebox.caret = arg;
                            }
                        }
                        if (io_log) log(prompt::key, "IME composition preview: ", ansi::hi(ansi::s11n(imebox.content(), rect{.size = imebox.size()})));
                        unsync = true;
                    }
                    else unsync = std::exchange(ime_on, imebox.length()) != ime_on;
                    break;
                case keybd::type::kblayout:
                    //todo
                    break;
            }
        }

    protected:
        // term: Recalc metrics for the new viewport size.
        void deform(rect& new_area) override
        {
            new_area += base::intpad;
            auto& console = *target;
            auto scroll_coor = origin;
            new_area.size = std::max(new_area.size, dot_11);
            console.resize_viewport(new_area.size);
            console.recalc_pads(base::oversz);
            scroll(origin);
            base::anchor += scroll_coor - origin;
            ipccon.resize(new_area.size);
            new_area.size.y += console.get_basis();
            new_area -= base::intpad;
        }

    public:
        term()
            : defcfg{ bell::indexer.config },
              normal{ *this },
              altbuf{ *this },
              target{ &normal },
               caret{ base::plugin<pro::caret>(defcfg.def_cur_on, defcfg.def_cursor, dot_00, defcfg.def_period, defcfg.def_curclr) },
               timer{ base::plugin<pro::timer>() },
               robot{ base::plugin<pro::robot>() },
              mtrack{ *this },
              ftrack{ *this },
              wtrack{ *this },
              ctrack{ *this },
              follow{ 0, 1 },
              insmod{ faux },
              decckm{ faux },
              bpmode{ faux },
              unsync{ faux },
              invert{ faux },
              styled{ faux },
              io_log{ defcfg.def_io_log },
              selalt{ defcfg.def_selalt },
              seldrag{ dragmode::none },
              resume{ faux },
              restart_pending{ faux },
              forced{ faux },
              selmod{ defcfg.def_selmod },
              onesht{ mime::disabled },
              altscr{ defcfg.def_alt_on },
              kbmode{ prot::vt },
              ime_on{ faux },
              rawkbd{ faux },
              bottom_anchored{ true },
              event_sources{}
        {
            set_fg_color(defcfg.def_fcolor);
            set_bg_color(defcfg.def_bcolor);
            selection_submit();
            selection_selmod(defcfg.def_selmod);

            auto& mouse = base::plugin<pro::mouse>();
            mouse.draggable<hids::buttons::left>(selection_passed());

            base::plugin<pro::keybd>();
            auto& luafx = bell::indexer.luafx;
            auto& config = bell::indexer.config;
            auto terminal_context = config.settings::push_context("/config/events/terminal/");
            auto script_list = config.settings::take_ptr_list_for_name("script");
            auto bindings = input::bindings::load(config, script_list);
            input::bindings::keybind(*this, bindings);
            base::add_methods(basename::terminal,
            {
                { methods::KeyEvent,                [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            auto backup = syskeybd{};
                                                            backup.set(gear);
                                                            auto args_count = luafx.args_count();
                                                            auto index = 0;
                                                            while (index < args_count)
                                                            {
                                                                //log("key args:");
                                                                auto k = syskeybd{};
                                                                k.syncto(gear);
                                                                gear.ctlstat = backup.ctlstat;
                                                                luafx.read_args(++index, [&](qiew key, qiew val)
                                                                {
                                                                         if (key == "keystat") gear.keystat = xml::take_or(val, input::key::released);
                                                                    else if (key == "ctlstat") gear.ctlstat = xml::take_or(val, backup.ctlstat);
                                                                    else if (key == "virtcod") gear.virtcod = xml::take_or(val, 0);
                                                                    else if (key == "scancod") gear.scancod = xml::take_or(val, 0);
                                                                    else if (key == "keycode") gear.keycode = xml::take_or(val, 0);
                                                                    else if (key == "extflag") gear.extflag = xml::take_or(val, 0);
                                                                    else if (key == "cluster") gear.cluster = val;
                                                                    else log("%%Unknown key event parameters %%=%%", prompt::lua, key, utf::debase437(val));
                                                                    //log("  %%=%%", key, utf::debase437(val));
                                                                });
                                                                key_event(gear);
                                                            }
                                                            backup.syncto(gear);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::ExclusiveKeyboardMode,   [&]
                                                    {
                                                        luafx.run_with_gear_wo_return([&](auto& gear){ gear.set_handled(); });
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            auto state = (si32)rawkbd;
                                                            luafx.set_return(state);
                                                        }
                                                        else
                                                        {
                                                            auto state = luafx.get_args_or(1, si32{ 0 });
                                                            set_rawkbd(1 + (si32)!!state);
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::FindNextMatch,           [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            auto dir = luafx.get_args_or(1, si32{ 1 });
                                                            selection_search(gear, dir > 0 ? feed::fwd : feed::rev);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::ScrollViewportByPage,    [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            if (target != &normal) return;
                                                            auto vector = luafx.get_args_or(1, dot_00);
                                                            base::riseup(tier::preview, e2::form::upon::scroll::bypage::v, { .vector = vector });
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::ScrollViewportByCell,    [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            if (target != &normal) return;
                                                            auto vector = luafx.get_args_or(1, dot_00);
                                                            base::riseup(tier::preview, e2::form::upon::scroll::bystep::v, { .vector = vector });
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::ScrollViewportToTop,     [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            if (target != &normal) return;
                                                            base::riseup(tier::preview, e2::form::upon::scroll::to_top::y);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::ScrollViewportToEnd,     [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            if (target != &normal) return;
                                                            base::riseup(tier::preview, e2::form::upon::scroll::to_end::y);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::SendKey,                 [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            auto crop = luafx.get_args_or(1, ""s);
                                                            if (crop.size()) data_out(crop);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::Print,                   [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            auto args_count = luafx.args_count();
                                                            auto crop = text{};
                                                            for (auto i = 1; i <= args_count; i++)
                                                            {
                                                                crop += luafx.get_args_or(i, ""s);
                                                            }
                                                            if (crop.size()) data_in(crop);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::PrintLn,                 [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            auto args_count = luafx.args_count();
                                                            auto crop = text{};
                                                            for (auto i = 1; i <= args_count; i++)
                                                            {
                                                                crop += luafx.get_args_or(i, ""s);
                                                            }
                                                            crop += "\n\r";
                                                            data_in(crop);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::CopyViewport,            [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            prnscrn(gear);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::CopySelection,           [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            if (selection_active())
                                                            {
                                                                copy(gear);
                                                                gear.set_handled();
                                                            }
                                                            else if (auto v = ipccon.get_current_line())
                                                            {
                                                                _copy(gear, v.value());
                                                                gear.set_handled();
                                                            }
                                                        });
                                                    }},
                { methods::PasteClipboard,          [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            paste(gear);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::ClearClipboard,          [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            gear.clear_clipboard();
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::ClipboardFormat,         [&]
                                                    {
                                                        luafx.run_with_gear_wo_return([&](auto& gear){ gear.set_handled(); });
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            auto state = (si32)selmod;
                                                            luafx.set_return(state);
                                                        }
                                                        else
                                                        {
                                                            auto state = luafx.get_args_or(1, si32{ 1 });
                                                            set_selmod(state % mime::count);
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::SelectionForm,           [&]
                                                    {
                                                        luafx.run_with_gear_wo_return([&](auto& gear){ gear.set_handled(); });
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            auto state = (si32)selalt;
                                                            luafx.set_return(state);
                                                        }
                                                        else
                                                        {
                                                            auto state = luafx.get_args_or(1, si32{ 0 });
                                                            set_selalt(state);
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::ClearSelection,          [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            if (selection_active())
                                                            {
                                                                exec_cmd(commands::ui::deselect);
                                                                gear.set_handled();
                                                            }
                                                        });
                                                    }},
                { methods::OneShotSelection,        [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& /*gear*/)
                                                        {
                                                            auto format = luafx.get_args_or(1, ""s);
                                                            if (format.empty())
                                                            {
                                                                set_oneshot(mime::textonly);
                                                            }
                                                            else
                                                            {
                                                                set_oneshot(netxs::get_or(xml::options::format, format, mime::textonly));
                                                            }
                                                        });
                                                    }},
                { methods::UndoReadline,            [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            exec_cmd(commands::ui::undo);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::RedoReadline,            [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            exec_cmd(commands::ui::redo);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::CwdSync,                 [&]
                                                    {
                                                        auto& cwd_sync = base::property("terminal.cwd_sync", 0);
                                                        luafx.run_with_gear_wo_return([&](auto& gear){ gear.set_handled(); });
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            auto state = (si32)cwd_sync;
                                                            luafx.set_return(state);
                                                        }
                                                        else
                                                        {
                                                            auto state = luafx.get_args_or(1, si32{ 0 });
                                                            base::riseup(tier::preview, terminal::events::toggle::cwdsync, state);
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::LineWrapMode,            [&]
                                                    {
                                                        luafx.run_with_gear_wo_return([&](auto& gear){ gear.set_handled(); });
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            auto state = target->style.wrp() == wrap::off ? 0 : 1;
                                                            luafx.set_return(state);
                                                        }
                                                        else
                                                        {
                                                            auto state = luafx.get_args_or(1, si32{ 1 });
                                                            set_wrapln(1 + (si32)!state);
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::LineAlignMode,           [&]
                                                    {
                                                        luafx.run_with_gear_wo_return([&](auto& gear){ gear.set_handled(); });
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            auto align = target->style.jet();
                                                            auto state = align == bias::left || align == bias::none  ? 0 :
                                                                                                align == bias::right ? 1 : 2;
                                                            luafx.set_return(state);
                                                        }
                                                        else
                                                        {
                                                            auto state = luafx.get_args_or(1, si32{ 0 });
                                                            set_align(1 + std::clamp(state, 0, 2));
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::LogMode,                 [&]
                                                    {
                                                        luafx.run_with_gear_wo_return([&](auto& gear){ gear.set_handled(); });
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            auto state = (si32)io_log;
                                                            luafx.set_return(state);
                                                        }
                                                        else
                                                        {
                                                            auto state = luafx.get_args_or(1, si32{ 0 });
                                                            set_log(state);
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::AltbufMode,              [&]
                                                    {
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            auto is_altbuf = target != &normal;
                                                            luafx.set_return(is_altbuf);
                                                        }
                                                        else
                                                        {
                                                            auto state = luafx.get_args_or(1, faux);
                                                            state ? _decset(1049) : _decrst(1049);
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::ForwardKeys,             [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            key_event(gear, true);
                                                        });
                                                    }},
                { methods::ClearScrollback,         [&]
                                                    {
                                                        luafx.run_with_gear_wo_return([&](auto& gear){ gear.set_handled(); });
                                                        if (selection_active())
                                                        {
                                                            exec_cmd(commands::ui::deselect);
                                                        }
                                                        clear_scrollback();
                                                        luafx.set_return();
                                                    }},
                { methods::ScrollbackSize,          [&]
                                                    {
                                                        luafx.run_with_gear_wo_return([&](auto& gear){ gear.set_handled(); });
                                                        target->flush();
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            luafx.set_return(defcfg.def_length, defcfg.def_growdt, defcfg.def_growmx);
                                                        }
                                                        else
                                                        {
                                                            auto ring_size = luafx.get_args_or(1, defcfg.def_length);
                                                            auto grow_step = luafx.get_args_or(2, defcfg.def_growdt);
                                                            auto grow_mxsz = luafx.get_args_or(3, defcfg.def_growmx);
                                                            normal.resize_history(ring_size, grow_step, grow_mxsz);
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::SetBackground,           [&]
                                                    {
                                                        target->flush();
                                                        auto brush = target->brush;
                                                        set_color(brush.txt('\0'));
                                                        luafx.set_return();
                                                    }},
                { methods::ScrollbackPadding,       [&]
                                                    {
                                                        target->flush();
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            luafx.set_return(target->getpad());
                                                        }
                                                        else
                                                        {
                                                            auto padding = luafx.get_args_or(1, 0);
                                                            target->setpad(padding);
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::TabLength,               [&]
                                                    {
                                                        target->flush();
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            luafx.set_return(defcfg.def_tablen);
                                                        }
                                                        else
                                                        {
                                                            auto tablen = std::clamp(luafx.get_args_or(1, 8), 1, 256);
                                                            defcfg.def_tablen = tablen;
                                                            target->rtb();
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::RightToLeft,             [&]
                                                    {
                                                        target->flush();
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            luafx.set_return(!!target->brush.rtl());
                                                        }
                                                        else
                                                        {
                                                            auto rtl = luafx.get_args_or(1, 0);
                                                            target->style.rtl(rtl ? rtol::rtl : rtol::ltr);
                                                            target->brush.rtl(rtl);
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::ResetAttributes,         [&]
                                                    {
                                                        target->flush();
                                                        setdef();
                                                        luafx.set_return();
                                                    }},
                { methods::EventReporting,          [&]
                                                    {
                                                        luafx.run_with_gear_wo_return([&](auto& gear){ gear.set_handled(); });
                                                        auto args_count = luafx.args_count();
                                                        if (!args_count)
                                                        {
                                                            auto src_list = txts{};
                                                            auto i = 0;
                                                            auto e = event_sources;
                                                            while (e)
                                                            {
                                                                if (e & 1) src_list.push_back(event_source_name[i]);
                                                                i++;
                                                                e >>= 1;
                                                            }
                                                            luafx.set_return_array(src_list);
                                                        }
                                                        else
                                                        {
                                                            auto prev_event_sources = event_sources;
                                                            for (auto i = 1; i <= args_count; i++)
                                                            {
                                                                auto src = luafx.get_args_or(i, ""s);
                                                                if (src.empty())
                                                                {
                                                                    event_sources = {};
                                                                }
                                                                else
                                                                {
                                                                    auto iter = ui::terminal::event_source_map.find(src);
                                                                    if (iter != ui::terminal::event_source_map.end())
                                                                    {
                                                                        event_sources |= iter->second;
                                                                    }
                                                                    else
                                                                    {
                                                                        log("%%Unknown event source: '%%'", prompt::term, src);
                                                                    }
                                                                }
                                                            }
                                                            auto mouse_tracking = event_sources & ui::terminal::event_source::mouse;
                                                            if ((prev_event_sources & ui::terminal::event_source::mouse) != mouse_tracking)
                                                            {
                                                                base::enqueue([&, mouse_tracking](auto& /*boss*/) // Perform switching outside of Lua script context.
                                                                {
                                                                    mouse_tracking ? mtrack.enable(input::mouse::mode::vt_input_mode)
                                                                                   : mtrack.disable(input::mouse::mode::vt_input_mode);
                                                                });
                                                            }
                                                            luafx.set_return();
                                                        }
                                                    }},
                { methods::Restart,                 [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            exec_cmd(commands::ui::restart);
                                                            gear.set_handled();
                                                        });
                                                    }},
                { methods::CodePage,                [&]
                                                    {
                                                        target->flush();
                                                        if (!ipccon.termlink)
                                                        {
                                                            luafx.set_return();
                                                        }
                                                        else
                                                        {
                                                            auto args_count = luafx.args_count();
                                                            if (!args_count)
                                                            {
                                                                luafx.set_return(ipccon.termlink->get_cp());
                                                            }
                                                            else
                                                            {
                                                                if (auto codepage = luafx.get_args_or(1, 0))
                                                                {
                                                                    ipccon.termlink->set_cp(codepage);
                                                                }
                                                                luafx.set_return();
                                                            }
                                                        }
                                                    }},
                { methods::Quit,                    [&]
                                                    {
                                                        luafx.run_with_gear([&](auto& gear)
                                                        {
                                                            exec_cmd(commands::ui::sighup);
                                                            gear.set_handled();
                                                        });
                                                    }},
            });

            LISTEN(tier::general, e2::timer::tick, timestamp) // Update before world rendering.
            {
                if (unsync)
                {
                    unsync = faux;
                    auto& console = *target;
                    auto scroll_size = console.panel;
                    scroll_size.y += console.get_basis();
                    auto scroll_coor = origin;
                    scroll(scroll_coor);
                    auto adjust_pads = console.recalc_pads(base::oversz);
                    if (scroll_size != base::size() // Update scrollbars.
                     || scroll_coor != origin
                     || adjust_pads)
                    {
                        auto new_area = rect{ scroll_coor, scroll_size };
                        base::signal(tier::release, e2::area, new_area);
                        base::region = new_area;
                    }
                    base::deface();
                }
            };
            LISTEN(tier::release, ui::e2::command::request::inputfields, inputfield_request)
            {
                auto& console = *target;
                auto composit_cursor = console.get_coord(origin) + origin;
                if (auto parent = base::parent())
                {
                    auto parent_size = parent->size();
                    if (parent_size.inside(composit_cursor))
                    {
                        if (ime_on) composit_cursor = calc_composition_cursor(parent_size, composit_cursor);
                        auto r = rect{ composit_cursor, { parent_size.x - composit_cursor.x, 1 }};
                        auto offset = dot_00;
                        parent->global(offset);
                        r.coor -= offset;
                        inputfield_request.set_value(r);
                    }
                }
            };
            LISTEN(tier::release, e2::area, new_area)
            {
                if (new_area.coor != base::coor())
                {
                    auto& console = *target;
                    auto fresh_coor = -new_area.coor.y;
                    follow[axis::Y] = console.set_slide(fresh_coor);
                    new_area.coor.y = -fresh_coor;
                    origin = new_area.coor;
                }
            };
            LISTEN(tier::release, input::events::keybd::post, gear)
            {
                key_event(gear);
            };
            LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                auto& console = *target;
                if (status.update(console))
                {
                    base::riseup(tier::preview, e2::form::prop::ui::footer, status.data);
                }

                auto clip = parent_canvas.clip();
                auto full = parent_canvas.full();
                auto original_cursor = console.get_coord(origin); // base::coor() and origin are the same.

                auto brush = defclr;
                if (defcfg.def_filler != argb::default_color) brush.bgc(defcfg.def_filler); // Unsync with SGR default background.
                parent_canvas.fill(cell::shaders::fusefull(brush));

                if (ime_on) // Draw IME composition overlay.
                {
                    if (auto parent = base::parent())
                    {
                        brush.fuse(console.cell_under_cursor());
                        auto viewport_square = parent->area();
                        auto viewport_cursor = original_cursor + origin;
                        if (viewport_square.size.inside(viewport_cursor))
                        {
                            auto composit_cursor = calc_composition_cursor(viewport_square.size, viewport_cursor);

                            auto dy = std::max(0, composit_cursor.y - (viewport_square.size.y - 1));
                            if (dy) // Shift parent_canvas down if needed.
                            {
                                auto scrolldown = full;
                                composit_cursor.y -= dy;
                                viewport_cursor.y -= dy;
                                scrolldown.coor.y -= dy;
                                parent_canvas.full(scrolldown);
                            }
                            console.output(parent_canvas);
                            if (dy) parent_canvas.full(full); // Return parent_canvas back.

                            viewport_square.coor -= origin;
                            if (auto context2D = parent_canvas.change_basis(viewport_square))
                            {
                                parent_canvas.output<faux>(imebox, viewport_cursor, cell::shaders::mimic(brush));
                            }
                            composit_cursor -= origin; // Convert to original (scrollback based) basis.
                            caret.coor(composit_cursor);
                        }
                        else // Original cursor is outside the viewport.
                        {
                            caret.coor(original_cursor);
                            console.output(parent_canvas);
                        }
                    }
                }
                else
                {
                    caret.coor(original_cursor);
                    if (brush.bga() != 0xFF) parent_canvas.fill(rect{ caret.coor(), dot_11 }, [&](cell& c){ c.fgc(console.brush.fgc()); }); // Prefill the cursor cell placeholder in the case of transparent background.
                    console.output(parent_canvas);
                }
                if (invert) parent_canvas.fill(cell::shaders::invbit);

                if (oversz.b > 0) // Shade the viewport bottom oversize (futures).
                {
                    auto bottom_oversize = full;
                    bottom_oversize.coor.x -= oversz.l;
                    bottom_oversize.coor.y += console.get_basis() + console.panel.y - console.scend;
                    bottom_oversize.size.y  = oversz.b;
                    bottom_oversize.size.x += oversz.l + oversz.r;
                    bottom_oversize = bottom_oversize.trim(clip);
                    parent_canvas.fill(bottom_oversize, cell::shaders::xlight);
                }

                //if (clip.coor.x) // Shade left and right margins.
                //{
                //    auto west = full;
                //    west.size = dot_mx;
                //    west.coor.y -= dot_mx.y / 2;
                //    auto east = west;
                //    auto pads = console.getpad();
                //    west.coor.x -= oversz.l - pads + dot_mx.x;
                //    east.coor.x += oversz.r - pads + console.panel.x;
                //    west = west.trim(clip);
                //    east = east.trim(clip);
                //    parent_canvas.fill(west, cell::shaders::xlucent(defcfg.def_lucent));
                //    parent_canvas.fill(east, cell::shaders::xlucent(defcfg.def_lucent));
                //}

                // Debug: Shade active viewport.
                //{
                //    auto size = console.panel;
                //    size.y -= console.sctop + console.scend;
                //    auto vp = rect{{ 0,console.get_basis() + console.sctop }, size };
                //    vp.coor += parent_canvas.full().coor;
                //    vp = vp.clip(parent_canvas.clip());
                //    parent_canvas.fill(vp, [](auto& c){ c.fuse(cell{}.bgc(magentalt).bga(50)); });
                //}
            };
        }
