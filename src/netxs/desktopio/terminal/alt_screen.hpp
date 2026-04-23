        // term: Alternate screen buffer implementation.
        struct alt_screen
            : public bufferbase
        {
            struct dragspan
            {
                twod head{};
                twod tail{};
                bool ready{};
            };

            rich canvas; // alt_screen: Terminal screen.
            twod seltop; // alt_screen: Selected area head.
            twod selend; // alt_screen: Selected area tail.
            dragspan dragbase;

            alt_screen(term& boss)
                : bufferbase{ boss }
            { }

            si32 get_size() const override { return panel.y; }
            si32 get_peak() const override { return panel.y; }
            si32 get_mxsz() const override { return panel.y; }
            si32 get_step() const override { return 0;       }

            // alt_screen: Resize viewport.
            void resize_viewport(twod new_sz, bool /*forced*/ = faux) override
            {
                bufferbase::resize_viewport(new_sz);
                coord = std::clamp(coord, dot_00, panel - dot_11);
                canvas.crop(panel, brush.spare.dry());
            }
            // alt_screen: Return viewport height.
            si32 height() override
            {
                return panel.y;
            }
            // alt_screen: Recalc left and right oversize (Always 0 for altbuf).
            bool recalc_pads(dent& new_oversz) override
            {
                auto left = 0;
                auto rght = 0;
                if (new_oversz.r != rght
                 || new_oversz.l != left)
                {
                    new_oversz.r = rght;
                    new_oversz.l = left;
                    return true;
                }
                else return faux;
            }
            static void _el(si32 n, core& canvas, twod coord, twod panel, cell const& blank)
            {
                assert(coord.y < panel.y);
                assert(coord.x >= 0);
                auto size = canvas.size();
                auto head = canvas.begin() + coord.y * size.x;
                auto tail = head;
                switch (n)
                {
                    default:
                    case commands::erase::line::right: // n = 0 (default)  Erase to Right.
                        head += std::min(panel.x, coord.x);
                        tail += panel.x;
                        break;
                    case commands::erase::line::left: // n = 1  Erase to Left.
                        tail += std::min(panel.x, coord.x + 1); // +1 to include the current cell.
                        break;
                    case commands::erase::line::all: // n = 2  Erase All.
                        tail += panel.x;
                        break;
                }
                while (head != tail) *head++ = blank;
            }
            // alt_screen: CSI n K  Erase line (don't move cursor).
            void el(si32 n) override
            {
                bufferbase::flush();
                _el(n, canvas, coord, panel, brush.spc()); // ok
            }
            // alt_screen: CSI n @  ICH. Insert n colored blanks after cursor. No wrap. Existing chars after cursor shifts to the right. Don't change cursor pos.
            void ins(si32 n) override
            {
                bufferbase::flush();
                assert(coord.y < panel.y);
                assert(coord.x >= 0);
                canvas.insert(coord, n, brush.spc());
            }
            // alt_screen: CSI n P  Delete (not Erase) letters under the cursor by defclr.
            void dch(si32 n) override
            {
                bufferbase::flush();
                canvas.cutoff(coord, n, brush.spare.spc());
            }
            // alt_screen: '\x7F'  Delete letter backward.
            void del(si32 n) override
            {
                bufferbase::flush();
                n = std::max(0, n);
                coord.x -= n;
                if (coord.x < 0)
                {
                    wrapup();
                }
                canvas.backsp(coord, n, brush.spare.spc());
                if (coord.y < 0) coord = dot_00;
            }
            // alt_screen: Move cursor by n in line.
            void move(si32 n) override
            {
                bufferbase::flush();
                coord.x += n;
                if      (coord.x < 0)       wrapup();
                else if (coord.x > panel.x) wrapdn();
                if      (coord.y < 0)        coord = dot_00;
                else if (coord.y >= panel.y) coord = panel - dot_01;
            }
            // alt_screen: CSI n X  Erase/put n chars after cursor. Don't change cursor pos.
            void ech(si32 n, char c = '\0') override
            {
                parser::flush();
                auto blank = brush;
                blank.txt(c);
                canvas.splice(coord, n, blank);
            }
            // alt_screen: Proceed new text using specified cell shader.
            template<bool Copy = faux, class Span, class Shader>
            void _data(si32 count, Span const& proto, Shader fuse)
            {
                assert(coord.y >= 0 && coord.y < panel.y);

                auto start = coord;
                coord.x += count;
                //todo apply line adjusting (necessity is not clear)
                if (coord.x <= panel.x)//todo styles! || ! curln.wrapped())
                {
                    auto n = std::min(count, panel.x - std::max(0, start.x));
                    canvas.splice<Copy>(start, n, proto, fuse);
                }
                else
                {
                    wrapdn();
                    if (start.y < y_top)
                    {
                        if (coord.y >= y_top)
                        {
                            auto n = coord.x + (coord.y - y_top) * panel.x;
                            count -= n;
                            set_coord({ 0, y_top });
                            _data<Copy>(n, proto, fuse); // Reversed fill using the last part of the proto.
                        }
                        auto data = proto.begin();
                        auto seek = start.x + start.y * panel.x;
                        auto dest = canvas.begin() + seek;
                        auto tail = dest + count;
                        rich::forward_fill_proc<Copy>(data, dest, tail, fuse);
                    }
                    else if (start.y <= y_end)
                    {
                        if (coord.y > y_end)
                        {
                            auto dy = y_end - coord.y;
                            coord.y = y_end;
                            canvas.scroll(y_top, y_end + 1, dy, brush.spare);
                        }

                        auto seek = coord.x + coord.y * panel.x;
                        auto miny = seek - y_top * panel.x;
                        if (count > miny) count = miny;

                        auto dest = canvas.begin() + seek;
                        auto tail = dest - count;
                        auto data = proto.end();
                        rich::reverse_fill_proc<Copy>(data, dest, tail, fuse);
                    }
                    else
                    {
                        if (coord.y >= panel.y) coord.y = panel.y - 1;

                        auto data = proto.begin();
                        auto size = count;
                        auto seek = start.x + start.y * panel.x;
                        auto dest = canvas.begin() + seek;
                        auto tail = canvas.end();
                        auto back = panel.x;
                        rich::unlimit_fill_proc<Copy>(data, size, dest, tail, back, fuse);
                    }
                }
            }
            // alt_screen: .
            auto& _fragment_from_current_coord(si32 left_cells)
            {
                canvas.copy_piece(tail_frag, coord.x + coord.y * panel.x, left_cells);
                return tail_frag;
            }
            // alt_screen: .
            template<bool Copy, class Span, class Shader>
            void _data_direct_fill(si32 count, Span const& proto, Shader fuse)
            {
                auto fill = [&](auto start_iter, auto seek)
                {
                    auto dest = start_iter + seek;
                    assert(count <= panel.x - coord.x);
                    auto tail = dest + count;
                    auto data = proto.begin();
                    rich::forward_fill_proc<Copy>(data, dest, tail, fuse);
                };
                fill(canvas.begin(), coord.x + coord.y * panel.x);
            }
            // alt_screen: Insert new text using the specified cell shader.
            template<class Span, class Shader>
            void _data_insert(si32 count, Span const& proto, Shader fuse)
            {
                auto next_x = coord.x + count;
                if (next_x < panel.x)
                {
                    auto left_cells = panel.x - next_x;
                    tail_frag = _fragment_from_current_coord(left_cells);
                    _data_direct_fill<faux>(count, proto, fuse);
                    coord.x = next_x;
                    if (tail_frag.size())
                    {
                        _data_direct_fill<true>(tail_frag.length(), tail_frag, fuse);
                    }
                }
                else
                {
                    _data(count, proto, fuse);
                }
            }
            // alt_screen: Parser callback.
            void data(si32 width, si32 height, core::body const& proto) override
            {
                if (width)
                {
                    if (height == 1)
                    {
                        owner.insmod ? _data_insert(width, proto, cell::shaders::skipnulls)
                                     : _data(width, proto, cell::shaders::skipnulls);
                    }
                    else // We do not support insmod for data_2d().
                    {
                        data_2d({ width, height }, proto, [&](auto& l){ _data<true>((si32)l.size(), l, cell::shaders::skipnulls); });
                    }
                }
            }
            // alt_screen: Clear viewport using current brush.
            void clear_all() override
            {
                canvas.wipe(brush.dry()); // ok
                set_scroll_region(0, 0);
                bufferbase::clear_all();
            }
            // alt_screen: Render to the dest.
            void output(face& dest) override
            {
                auto full = dest.full();
                auto find = selection_active()
                         && match.length()
                         && owner.selmod == mime::textonly;
                canvas.move(full.coor - dest.coor());
                dest.plot(canvas, cell::shaders::flat);
                if (auto area = canvas.area())
                {
                    if (find)
                    {
                        auto offset = si32{};
                        auto work = [&](auto shader)
                        {
                            while (canvas.find(match, offset))
                            {
                                auto c = canvas.toxy(offset);
                                dest.output(match, c, shader);
                                offset += match.length();
                            }
                        };
                        _shade(owner.defcfg.def_find_f, owner.defcfg.def_find_c, work);
                    }
                    selection_render(dest);
                }
            }
            // alt_screen: Clear all lines below except the current by the current brush . "ED2 Erase viewport" keeps empty lines.
            void del_below() override
            {
                canvas.del_below(coord, brush.dry());
            }
            // alt_screen: Clear all lines from the viewport top line to the current line by the current brush.
            void del_above() override
            {
                auto coorx = coord.x;
                if (coorx < panel.x) ++coord.x; // Clear the cell at the current position. See ED1 description.
                canvas.del_above(coord, brush.dry());
                coord.x = coorx;
            }
            // alt_screen: Shift by n the scroll region.
            void scroll_region(si32 top, si32 end, si32 n, [[maybe_unused]] bool use_scrollback = faux) override
            {
                seltop.y += n;
                selend.y += n;
                if (dragbase.ready)
                {
                    dragbase.head.y += n;
                    dragbase.tail.y += n;
                }
                canvas.scroll(top, end + 1, n, cell{ '\0' }.bgc(brush.bgc()).link(brush.link())); // We use "BCE on scrolling" in altbuf mode only (vim).
            }
            // alt_screen: Horizontal tab.
            void tab(si32 n) override
            {
                bufferbase::tab(n);
                coord.x = std::clamp(coord.x, 0, panel.x - 1);
            }
            // alt_screen: Make a viewport screen copy.
            void do_viewport_copy(face& dest) override
            {
                auto full = dest.full();
                auto clip = dest.clip().trim(full);
                dest.clip(clip);
                dest.plot(canvas, cell::shaders::full);
            }
            // alt_screen: Return cell state under cursor.
            cell cell_under_cursor() override
            {
                auto coor = std::clamp(coord, dot_00, panel - dot_11);
                auto c = canvas[coor];
                return c;
            }
            // alt_screen: Clear scrollback keeping current line.
            void clear_scrollback() override
            {
                if (coord.y > 0 && panel.y > 1)
                {
                    scroll_region(0, panel.y - 1, -coord.y);
                }
                canvas.del_below({ 0, 1 }, brush.spare.dry());
                set_coord({ coord.x, 0 });
            }
            // alt_screen: Snap linear selection edges to complete grapheme boundaries.
            void normalize_linear_edges(twod& edge_1, twod& edge_2)
            {
                if (panel.x <= 0 || panel.y <= 0) return;
                auto limits = panel - dot_11;
                edge_1 = std::clamp(edge_1, dot_00, limits);
                edge_2 = std::clamp(edge_2, dot_00, limits);

                auto to_offset = [&](auto p)
                {
                    return p.x + p.y * panel.x;
                };
                auto to_coord = [&](auto offset)
                {
                    return twod{ offset % panel.x, offset / panel.x };
                };
                auto cells = panel.x * panel.y;
                auto head = to_offset(edge_1);
                auto tail = to_offset(edge_2);
                auto swap = head > tail;
                if (swap) std::swap(head, tail);

                auto normalize = [&](auto& offset, auto start_edge)
                {
                    offset = std::clamp(offset, 0, cells - 1);
                    auto const& c = *(canvas.begin() + offset);
                    auto [w, h, x, y] = c.whxy();
                    if (h == 1 && w > 1)
                    {
                        if (start_edge && x > 1) offset -= x - 1;
                        else if (!start_edge && x < w) offset += w - x;
                        offset = std::clamp(offset, 0, cells - 1);
                    }
                };
                normalize(head, true );
                normalize(tail, faux);

                if (swap)
                {
                    edge_1 = to_coord(tail);
                    edge_2 = to_coord(head);
                }
                else
                {
                    edge_1 = to_coord(head);
                    edge_2 = to_coord(tail);
                }
            }
            //text get_current_line() override
            //{
            //    auto crop = escx{};
            //    auto cy = std::clamp(coord.y, 0, panel.y - 1);
            //    auto p1 = twod{ 0, cy };
            //    auto p2 = twod{ panel.x, cy };
            //    auto stripe = canvas.line(p1, p2);
            //    auto brush_state = cell{};
            //    if (owner.selmod == mime::textonly
            //     || owner.selmod == mime::safetext
            //     || owner.selmod == mime::disabled)
            //    {
            //        utf::trim(crop.s11n<faux, true, faux>(stripe, brush_state));
            //    }
            //    else
            //    {
            //        crop.s11n<true, true, faux>(stripe, brush_state);
            //    }
            //    return crop;
            //}

            // alt_screen: Start text selection.
            void selection_create(twod coor, bool mode) override
            {
                auto limits = panel - dot_11;
                seltop = std::clamp(coor, dot_00, limits);
                selend = seltop;
                selection_selbox(mode);
                selection_update();
            }
            // alt_screen: Extend text selection.
            bool selection_extend(twod coor, bool mode) override
            {
                auto ok = selection_active();
                if (ok)
                {
                    auto limits = panel - dot_11;
                    selend = std::clamp(coor, dot_00, limits);
                    selection_selbox(mode);
                    selection_update();
                }
                return ok;
            }
            // alt_screen: Set selection orientation.
            void selection_follow(twod coor, bool lock) override
            {
                selection_locked(lock);
                if (selection_active())
                {
                    auto limits = panel - dot_11;
                    coor = std::clamp(coor, dot_00, limits);
                    if (selection_selbox())
                    {
                        auto c = (selend + seltop) / 2;
                        if ((coor.x > c.x) == (seltop.x > selend.x)) std::swap(seltop.x, selend.x);
                        if ((coor.y > c.y) == (seltop.y > selend.y)) std::swap(seltop.y, selend.y);
                    }
                    else
                    {
                        auto swap = selend.y == seltop.y ? std::abs(selend.x - coor.x) > std::abs(seltop.x - coor.x)
                                                         : std::abs(selend.y - coor.y) > std::abs(seltop.y - coor.y);
                        if (swap) std::swap(seltop, selend);
                    }
                }
            }
            // alt_screen: Select one word.
            void selection_byword(twod coor) override
            {
                seltop = selend = coor;
                seltop.x = canvas.word<feed::rev>(coor);
                selend.x = canvas.word<feed::fwd>(coor);
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            void selection_byword(twod from, twod to) override
            {
                auto limits = panel - dot_11;
                auto locate = [&](twod coor)
                {
                    coor = std::clamp(coor, dot_00, limits);
                    auto head = coor;
                    auto tail = coor;
                    head.x = canvas.word<feed::rev>(coor);
                    tail.x = canvas.word<feed::fwd>(coor);
                    return std::pair{ head, tail };
                };
                auto earlier = [](twod const& a, twod const& b)
                {
                    return a.y < b.y
                        || (a.y == b.y && a.x < b.x);
                };
                auto [head_1, tail_1] = locate(from);
                auto [head_2, tail_2] = locate(to);
                seltop = earlier(head_2, head_1) ? head_2 : head_1;
                selend = earlier(tail_1, tail_2) ? tail_2 : tail_1;
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            // alt_screen: Select line.
            void selection_byline(twod coor) override
            {
                selection_byline(coor, coor);
            }
            void selection_byline(twod from, twod to) override
            {
                auto limits = panel - dot_11;
                from = std::clamp(from, dot_00, limits);
                to   = std::clamp(to,   dot_00, limits);
                seltop.y = std::min(from.y, to.y);
                selend.y = std::max(from.y, to.y);
                seltop.x = 0;
                selend.x = panel.x - 1;
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            void selection_drag_word_start(twod coor) override
            {
                selection_byword(coor);
                dragbase = { .head = seltop, .tail = selend, .ready = true };
            }
            void selection_drag_word_pull(twod coor) override
            {
                if (!dragbase.ready)
                {
                    selection_drag_word_start(coor);
                    return;
                }
                auto earlier = [](twod const& a, twod const& b)
                {
                    return a.y < b.y
                        || (a.y == b.y && a.x < b.x);
                };
                selection_byword(coor);
                auto edge = dragspan{ .head = seltop, .tail = selend, .ready = true };
                seltop = earlier(edge.head, dragbase.head) ? edge.head : dragbase.head;
                selend = earlier(dragbase.tail, edge.tail) ? edge.tail : dragbase.tail;
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            void selection_drag_line_start(twod coor) override
            {
                selection_byline(coor);
                dragbase = { .head = seltop, .tail = selend, .ready = true };
            }
            void selection_drag_line_pull(twod coor) override
            {
                if (!dragbase.ready)
                {
                    selection_drag_line_start(coor);
                    return;
                }
                auto earlier = [](twod const& a, twod const& b)
                {
                    return a.y < b.y
                        || (a.y == b.y && a.x < b.x);
                };
                selection_byline(coor);
                auto edge = dragspan{ .head = seltop, .tail = selend, .ready = true };
                seltop = earlier(edge.head, dragbase.head) ? edge.head : dragbase.head;
                selend = earlier(dragbase.tail, edge.tail) ? edge.tail : dragbase.tail;
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            void selection_drag_clear() override
            {
                dragbase.ready = faux;
            }
            // alt_screen: Select all.
            void selection_selall() override
            {
                seltop.y = 0;
                seltop.x = 0;
                selend = panel - dot_11;
                selection_locked(faux);
                selection_selbox(true);
                selection_update(faux);
            }
            // alt_screen: Take selected data.
            text selection_pickup(si32 selmod) override
            {
                auto data = escx{};
                if (selection_active())
                {
                    auto selbox = selection_selbox();
                    bufferbase::selection_pickup(data, canvas, seltop, selend, selmod, selbox);
                    if (selbox && !data.empty()) data.eol();
                }
                return data;
            }
            // alt_screen: Highlight selection.
            void selection_render(face& dest) override
            {
                auto clip = dest.clip();
                auto limits = panel - dot_11;
                auto curtop = std::clamp(seltop, dot_00, limits) - twod{ clip.coor.x, 0 }; // Compensate scrollback's hz movement.
                auto curend = std::clamp(selend, dot_00, limits) - twod{ clip.coor.x, 0 }; //
                selection_raster(dest, curtop, curend);
            }
            // alt_screen: Update selection status.
            void selection_status(term_state& status) const override
            {
                status.coor = std::abs(selend - seltop);
                status.body = status.coor.y * panel.x + status.coor.x + 1;
                status.coor+= dot_11;
            }
            // alt_screen: Update selection internals.
            void selection_update(bool despace = true) override
            {
                if (!selection_selbox())
                {
                    normalize_linear_edges(seltop, selend);
                }
                if (selection_selbox()
                 && seltop.y != selend.y)
                {
                    match = {};
                    uirev = faux;
                    uifwd = faux;
                }
                else
                {
                    match = { canvas.core::line(seltop, selend) };
                    auto p1 = seltop;
                    auto p2 = selend;
                    if (p1.y > p2.y || (p1.y == p2.y && p1.x > p2.x)) std::swap(p1, p2);
                    auto offset = p1.x + p1.y * panel.x;
                    uifwd = canvas.find(match, offset + match.length(), feed::fwd); // Try to find next next.
                    uirev = canvas.find(match, offset - 1,              feed::rev); // Try to find next prev.
                }
                bufferbase::selection_update(despace);
            }
            // alt_screen: Search data and return distance to it.
            twod selection_gofind(feed direction, view data = {}) override
            {
                if (data.empty()) return dot_00;
                match = line{ data };
                seltop = direction == feed::fwd ? twod{-match.length(), 0 }
                                                : twod{ panel.x, panel.y - 1 };
                selend = seltop;
                uirev = faux;
                uifwd = faux;
                selection_gonext(direction);
                if ((direction == feed::fwd && uirev == faux)
                 || (direction == feed::rev && uifwd == faux))
                {
                    selection_cancel();
                }
                return dot_00;
            }
            // alt_screen: Search prev/next selection match and return distance to it.
            twod selection_gonext(feed direction) override
            {
                auto p1 = seltop;
                auto p2 = selend;
                if (p1.y > p2.y || (p1.y == p2.y && p1.x > p2.x)) std::swap(p1, p2);

                auto from = p1.x + p1.y * panel.x;
                bufferbase::selection_search(canvas, from, direction, seltop, selend);
                bufferbase::selection_update(faux);
                return dot_00;
            }
            // alt_screen: Cancel text selection.
            bool selection_cancel() override
            {
                selection_drag_clear();
                bufferbase::uirev = faux;
                bufferbase::uifwd = faux;
                return bufferbase::selection_cancel();
            }
            // alt_screen: Count total occurrences of `query` in the alt canvas.
            si32 selection_count_matches(view query) override
            {
                if (query.empty()) return 0;
                auto probe = line{ query };
                auto mlen  = probe.length();
                if (!mlen) return 0;
                auto total = si32{ 0 };
                auto offset = si32{ 0 };
                while (canvas.find(probe, offset))
                {
                    ++total;
                    offset += mlen;
                }
                return total;
            }
        };
