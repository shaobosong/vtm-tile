        // term: Scrollback buffer implementation.
        struct scroll_buf
            : public bufferbase
        {
            struct index_item
            {
                using id_t = line::id_t;
                id_t index;
                si32 start;
                si32 width;
                index_item() = default;
                index_item(id_t index, si32 start, si32 width)
                    : index{ index },
                      start{ start },
                      width{ width }
                { }
            };
            struct grip
            {
                enum type
                {
                    idle,
                    base,
                    join,
                };
                id_t link{}; // scroll_buf::grip: Anchor id.
                twod coor{}; // scroll_buf::grip: 2D offset relative to link.
                type role{}; // scroll_buf::grip: Grip category.

                static void sort(grip& g1, grip& g2)
                {
                    if (g1.coor.y > g2.coor.y
                    || (g1.coor.y == g2.coor.y && g1.coor.x > g2.coor.x))
                    {
                        std::swap(g1, g2);
                    }
                }
            };
            enum class part
            {
                top, mid, end,
            };
            struct selspan
            {
                part place;
                grip upmid;
                grip dnmid;
                grip uptop;
                grip dntop;
                grip upend;
                grip dnend;
                twod head;
                twod tail;
            };
            using ring = generics::ring<std::vector<line>, true>;
            using indx = generics::ring<std::vector<index_item>>;

            struct buff : public ring
            {
                using ring::ring;
                using type = line::type;
                using maps = std::map<si32, si32>[type::count];

                si32 caret{}; // buff: Current line cursor horizontal position.
                si32 vsize{}; // buff: Scrollback vertical size (height).
                si32 width{}; // buff: Viewport width.
                si32 basis{}; // buff: Working area basis. Vertical position of O(0, 0) in the scrollback.
                si32 slide{}; // buff: Viewport vertical position in the scrollback.
                maps sizes{}; // buff: Line length accounting database.
                id_t ancid{}; // buff: The nearest line id to the slide.
                si32 ancdy{}; // buff: Slide's top line offset.
                bool round{}; // buff: Is the slide position approximate.
                bool rolls{}; // buff: The scrollback buffer ring was scrolled.

                // buff: Decrease height.
                void dec_height(si32& block_vsize, type line_kind, si32 line_size)
                {
                    if (line_size > width && line_kind == type::autowrap) block_vsize -= (line_size + width - 1) / width;
                    else                                                  block_vsize -= 1;
                }
                // buff: Increase height.
                void add_height(si32& block_vsize, type line_kind, si32 line_size)
                {
                    if (line_size > width && line_kind == type::autowrap) block_vsize += (line_size + width - 1) / width;
                    else                                                  block_vsize += 1;
                }
                // buff: Return max line length of the specified type.
                template<auto N>
                auto max()
                {
                    return sizes[N].empty() ? 0
                                            : sizes[N].rbegin()->first;
                }
                // buff: Recalculate unlimited scrollback height without reflow.
                void set_width(si32 new_width)
                {
                    vsize = 0;
                    width = std::max(1, new_width);
                    for (auto kind : { type::leftside,
                                       type::rghtside,
                                       type::centered })
                    {
                        for (auto [line_size, pool] : sizes[kind])
                        {
                            assert(pool > 0);
                            vsize += pool;
                        }
                    }
                    auto kind = type::autowrap;
                    for (auto [line_size, pool] : sizes[kind])
                    {
                        if (line_size > width) vsize += pool * ((line_size + width - 1) / width);
                        else                   vsize += pool;
                    }
                }
                // buff: Register a new line.
                void invite(type& line_kind, si32& line_size, type new_kind, si32 new_size)
                {
                    ++sizes[new_kind][new_size];
                    add_height(vsize, new_kind, new_size);
                    line_size = new_size;
                    line_kind = new_kind;
                }
                // buff: Refresh scrollback height.
                void recalc(type& line_kind, si32& line_size, type new_kind, si32 new_size)
                {
                    if (line_size != new_size
                     || line_kind != new_kind)
                    {
                        undock(line_kind, line_size);
                        ++sizes[new_kind][new_size];
                        add_height(vsize, new_kind, new_size);
                        line_size = new_size;
                        line_kind = new_kind;
                    }
                }
                // buff: Discard the specified metrics.
                void undock(type line_kind, si32 line_size)
                {
                    auto& lens = sizes[line_kind];
                    auto  iter = lens.find(line_size); assert(iter != lens.end());
                    auto  pool = --(*iter).second;
                    if (pool == 0) lens.erase(iter);
                    dec_height(vsize, line_kind, line_size);
                }
                // buff: Check buffer size.
                bool check_size(twod new_size)
                {
                    auto old_value = vsize;
                    set_width(new_size.x);
                    if (ring::peak <= new_size.y)
                    {
                        static constexpr auto BottomAnchored = true;
                        ring::resize<BottomAnchored>(new_size.y);
                    }
                    return old_value != vsize;
                }
                // buff: Push the specified line back.
                void invite(line& l)
                {
                    invite(l._kind, l._size, l.style.get_kind(), l.length());
                }
                // buff: Push a new line back.
                template<class ...Args>
                auto& invite(Args&&... args)
                {
                    auto& l = ring::push_back(std::forward<Args>(args)...);
                    invite(l._kind, l._size, l.style.get_kind(), l.length());
                    return l;
                }
                // buff: Insert a new line at the specified position.
                template<class ...Args>
                auto& insert(si32 at, Args&&... args)
                {
                    auto& l = *ring::insert(at, std::forward<Args>(args)...);
                    invite(l._kind, l._size, l.style.get_kind(), l.length());
                    return l;
                }
                // buff: Remove specified line info from accounting and update metrics based on scroll height.
                void undock_base_front(line& l) override
                {
                    auto line_kind = l._kind;
                    auto line_size = l._size;
                    undock(line_kind, line_size);
                    dec_height(basis, line_kind, line_size);
                    dec_height(slide, line_kind, line_size);
                    if (basis < 0)
                    {
                        basis = 0;
                    }
                    if (slide < 0)
                    {
                        ancid = l.index + 1;
                        ancdy = 0;
                        slide = 0;
                    }
                    rolls = true;
                }
                // buff: Remove information about the specified line from accounting.
                void undock_base_back(line& l) override { undock(l._kind, l._size); }
                // buff: Return the item position in the scrollback using its id.
                auto index_by_id(ui32 item_id) const
                {
                    //No need to disturb distant objects, it may already be in the swap.
                    auto total = length();
                    return (si32)(total - 1 - (back().index - item_id)); // ring buffer size is never larger than max_int32.
                }
                // buff: Return an iterator pointing to the item with the specified id.
                auto iter_by_id(ui32 line_id) -> ring::iter<ring> //todo MSVC 17.7.0 requires return type
                {
                    return begin() + index_by_id(line_id);
                }
                // buff: Return the item reference using its id.
                auto& item_by_id(ui32 line_id)
                {
                    return ring::at(index_by_id(line_id));
                }
                // buff: Refresh metrics due to modified line.
                void recalc(line& l)
                {
                    recalc(l._kind, l._size, l.style.get_kind(), l.length());
                }
                // buff: Rewrite the indices from the specified position to the end or to the top (negative from).
                void reindex(si32 from)
                {
                    if (from >= 0)
                    {
                        auto a = begin() + from;
                        auto b = end();
                        auto i = from == 0 ? 0 : (a - 1)->index + 1;
                        while (a != b)
                        {
                            a->index = i++;
                            ++a;
                        }
                    }
                    else
                    {
                        auto a = begin();
                        auto b = a + std::abs(from);
                        auto i = b->index - std::abs(from);
                        while (a != b)
                        {
                            a->index = i++;
                            ++a;
                        }
                    }
                }
                // buff: Remove the specified number of lines at the specified position (inclusive).
                auto remove(si32 at, si32 amount)
                {
                    amount = ring::remove(at, amount);
                    reindex(at);
                    return amount;
                }
                // buff: Clear scrollback, add one empty line, and reset all metrics.
                void clear()
                {
                    auto auto_wrap = current().wrapped();
                    ring::clear();
                    caret = 0;
                    basis = 0;
                    slide = 0;
                    invite(0, deco{}.wrp(auto_wrap), cell{}); // At least one line must exist.
                    ancid = back().index;
                    ancdy = 0;
                    set_width(width);
                }
                // buff: Clear scrollback keeping current line.
                void clear_but_current()
                {
                    auto backup = current();
                    backup.index = 0;
                    ring::clear();
                    auto& curln = ring::push_back(backup); // Keep current line.
                    basis = 0;
                    slide = 0;
                    invite(curln); // Sync current line state (length, wrap mode).
                    ancid = curln.index;
                    ancdy = 0;
                    set_width(width);
                }
            };

            friend auto& operator << (std::ostream& s, scroll_buf& c) // For debug.
            {
                return s << "{ " << c.batch.max<line::type::leftside>() << ","
                                 << c.batch.max<line::type::rghtside>() << ","
                                 << c.batch.max<line::type::centered>() << ","
                                 << c.batch.max<line::type::autowrap>() << " }";
            }

            buff batch; // scroll_buf: Scrollback container.
            indx index; // scroll_buf: Viewport line index.
            face upbox; // scroll_buf:    Top margin canvas.
            face dnbox; // scroll_buf: Bottom margin canvas.
            twod upmin; // scroll_buf:    Top margin minimal size.
            twod dnmin; // scroll_buf: Bottom margin minimal size.
            grip upmid; // scroll_buf: Selection first grip inside the scrolling region.
            grip dnmid; // scroll_buf: Selection second grip inside the scrolling region.
            grip uptop; // scroll_buf: Selection first grip inside the top margin.
            grip dntop; // scroll_buf: Selection second grip inside the top margin.
            grip upend; // scroll_buf: Selection first grip inside the bottom margin.
            grip dnend; // scroll_buf: Selection second grip inside the bottom margin.
            part place; // scroll_buf: Selection last active region.
            si32 shore; // scroll_buf: Left and right scrollbuffer additional indents.
            selspan dragbase{};
            bool    dragset{};

            static constexpr auto approx_threshold = si32{ 10000 }; //todo make it configurable

            scroll_buf(term& boss)
                : bufferbase{ boss },
                       batch{ boss.defcfg.def_length, boss.defcfg.def_growdt, boss.defcfg.def_growmx },
                       index{ 1    },
                       place{      },
                       shore{ boss.defcfg.def_margin },
                     dragset{ faux }
            {
                parser::style.wrp(boss.defcfg.def_wrpmod);
                batch.invite(0, deco{}.wrp(boss.defcfg.def_wrpmod == wrap::on), cell{}); // At least one line must exist.
                batch.set_width(1);
                index_rebuild();

                auto c = cell{ '\0' }.fgc(boss.defcfg.def_fcolor).bgc(boss.defcfg.def_bcolor).link(boss.id);
                boss.defclr = c;
                parser::brush.reset(c);
            }
            si32 get_size() const override { return batch.size;     }
            si32 get_peak() const override { return batch.peak - 1; }
            si32 get_step() const override { return batch.step;     }
            si32 get_mxsz() const override { return batch.mxsz;     }

            void print_slide(text msg)
            {
                log(msg, ": ", " batch.basis=", batch.basis, " batch.slide=", batch.slide,
                    " ancid=", batch.ancid, " ancdy=", batch.ancdy, " round=", batch.round ? 1:0);
            }
            void print_index(text msg)
            {
                log(" ", msg, " index.size=", index.size, " basis=", batch.basis, " panel=", panel);
                for (auto n = 0; auto& l : index)
                {
                    log("  ", n++,". id=", l.index," start=", l.start, " width=", l.width, l.start % panel.x != 0 ? " <-- BAD INDEX l.start % panel.x != 0":"");
                }
                auto& mapln = index.back();
                auto& curln = batch.item_by_id(mapln.index);
                log(" last ln id=", curln.index, " curln.length()=", curln.length());
                log(" -----------------");
            }
            void print_batch(text msg)
            {
                log(" ", msg,
                " batch.size=",  batch.size,
                " batch.vsize=", batch.vsize,
                " batch.basis=", batch.basis,
                " batch.slide=", batch.slide,
                " coord=", coord
                );
                for (auto n = 0; auto& l : batch)
                {
                    log("  ", n++,". id=", l.index, " length()=", l.length(), " wrapped=", l.wrapped() ? "true" : "faux");
                }
                log(" -----------------");
            }
            bool test_index()
            {
                #if defined(DEBUG)
                    auto m = index.front().index;
                    for (auto& i : index)
                    {
                        assert(i.index >= m && i.index - m < 2);
                        m = i.index;
                    }
                #endif
                return true;
            }
            bool test_futures()
            {
                auto stash = batch.vsize - batch.basis - index.size;
                if (stash < 0)
                {
                    print_batch("test_basis");
                    print_index("test_basis");
                }
                assert(stash >= 0);
                test_basis();
                return true;
            }
            bool test_coord()
            {
                assert(coord.y < y_top || coord.y > y_end || (batch.caret - coord.x/*wrapped_block*/) % panel.x == 0);
                return true;
            }
            auto test_resize()
            {
                #if defined(DEBUG)
                    auto c = batch.caret;
                    sync_coord();
                    assert(c == batch.caret);
                #else
                    sync_coord();
                #endif
                return true;
            }
            auto test_height()
            {
                auto test_vsize = 0;
                for (auto& l : batch)
                {
                    test_vsize += l.height(panel.x);
                }
                if (test_vsize != batch.vsize) log(" ERROR! test_vsize=", test_vsize, " vsize=", batch.vsize);
                return test_vsize == batch.vsize;
            }
            bool test_basis()
            {
                if (batch.basis >= batch.vsize)
                {
                    assert((log(" batch.basis >= batch.vsize  batch.basis=", batch.basis, " batch.vsize=", batch.vsize), true));
                }

                auto index_front = index.front();
                auto temp = index_front;
                auto coor = batch.vsize;
                auto head = batch.end();
                while (coor != batch.basis)
                {
                    auto& curln = *--head;
                    auto  curid = curln.index;
                    auto length = curln.length();
                    if (curln.wrapped())
                    {
                        auto remain = length ? (length - 1) % panel.x + 1 : 0;
                        length -= remain;
                        index_front = { curid, length, remain };
                        --coor;
                        while (length > 0 && coor != batch.basis)
                        {
                            length -= panel.x;
                            index_front = { curid, length, panel.x };
                            --coor;
                        }
                    }
                    else
                    {
                        index_front = { curid, 0, length };
                        --coor;
                    }
                }
                auto result = index_front.index == temp.index
                           && index_front.start == temp.start
                           && index_front.width == temp.width;
                if (!result)
                {
                    print_batch("test_basis");
                    print_index("test_basis");
                }
                assert(result);
                return result;
            }
            // scroll_buf: Get viewport position.
            si32 get_origin(bool follow) override
            {
                auto coor_y = follow ? batch.basis
                                     : batch.slide;
                scroll_buf::set_slide(coor_y); // Update slide id anchoring.
                return -coor_y;
            }
            // scroll_buf: Get viewport basis.
            si32 get_basis() override
            {
                return batch.basis;
            }
            // scroll_buf: Get viewport position.
            si32 get_slide() override
            {
                return batch.slide;
            }
            // scroll_buf: Set left/right scrollback additional padding.
            void setpad(si32 new_value) override
            {
                if (new_value < 0) new_value = owner.defcfg.def_margin;
                shore = std::min(new_value, 255);
            }
            // scroll_buf: Get left/right scrollback additional padding.
            si32 getpad() override
            {
                return shore;
            }
            // scroll_buf: Set viewport position and return whether the viewport is reset.
            bool set_slide(si32& fresh_slide) override
            {
                if (batch.slide == fresh_slide && !batch.rolls)
                {
                    return batch.slide == batch.basis;
                }
                batch.rolls = faux;

                if (batch.basis == fresh_slide)
                {
                    auto& mapln = index.front();
                    batch.ancid = mapln.index;
                    batch.ancdy = mapln.start / panel.x;
                    batch.slide = batch.basis;
                    batch.round = faux;
                }
                else
                {
                    auto& front = batch.front();
                    auto& under = batch.back();
                    auto  vtpos = batch.slide - batch.ancdy;
                    auto delta1 = fresh_slide - vtpos;       // Compare fresh_slide with batch.slide.
                    auto delta2 = fresh_slide - 0;           // Compare fresh_slide with 0.
                    auto delta3 = batch.vsize - fresh_slide; // Compare fresh_slide with batch.vsize.
                    auto range1 = std::abs(delta1);
                    auto range2 = std::abs(delta2);
                    auto range3 = std::abs(delta3);
                    auto lookup = [&]
                    {
                        auto idpos = batch.index_by_id(batch.ancid);
                        auto start = batch.begin() + idpos;
                        auto found = faux;
                        if (delta1 < 0) // Look up.
                        {
                            auto limit = start - std::min(range1, idpos);
                            while (start != limit)
                            {
                                auto& curln = *--start;
                                auto height = curln.height(panel.x);
                                vtpos -= height;
                                if (vtpos <= fresh_slide)
                                {
                                    batch.slide = fresh_slide;
                                    batch.ancid = curln.index;
                                    batch.ancdy = fresh_slide - vtpos;
                                    found = true;
                                    break;
                                }
                            }

                            if (!found)
                            {
                                batch.ancid = under.index - (batch.size - 1);
                                batch.ancdy = 0;
                                batch.slide = 0;
                                fresh_slide = 0;
                                batch.round = faux;
                                assert(batch.ancid == front.index);
                            }
                        }
                        else if (delta1 > 0) // Look down.
                        {
                            auto limit = start + std::min(delta1, batch.size - idpos - 1);
                            do
                            {
                                auto& curln = *start;
                                auto curpos = vtpos;
                                auto height = curln.height(panel.x);
                                vtpos += height;
                                if (vtpos > fresh_slide)
                                {
                                    batch.slide = fresh_slide;
                                    batch.ancid = curln.index;
                                    batch.ancdy = fresh_slide - curpos;
                                    found = true;
                                    break;
                                }
                            }
                            while (start++ != limit);

                            if (!found)
                            {
                                auto& mapln = index.front();
                                batch.slide = batch.basis;
                                fresh_slide = batch.basis;
                                batch.ancid = mapln.index;
                                batch.ancdy = mapln.start / panel.x;
                                batch.round = faux;
                            }
                        }
                        else
                        {
                            batch.slide = fresh_slide;
                            batch.ancdy = 0;
                            found = true;
                        }
                    };

                    if (batch.round && range1 < panel.y * 2)
                    {
                        lookup();
                        auto count1 = (si32)(under.index - batch.ancid);
                        auto count2 = (si32)(batch.ancid - front.index);
                        auto min_dy = std::min(count1, count2);

                        if (min_dy < approx_threshold) // Refine position to absolute value.
                        {
                            if (count1 < count2)
                            {
                                batch.slide = batch.ancdy + batch.vsize;
                                auto tail = batch.end();
                                auto head = tail - (count1 + 1);
                                while (head != tail)
                                {
                                    auto& curln = *--tail;
                                    batch.slide -= curln.height(panel.x);
                                }
                            }
                            else
                            {
                                batch.slide = batch.ancdy;
                                auto head = batch.begin();
                                auto tail = head + (count2 + 1);
                                while (head != tail)
                                {
                                    auto& curln = *head++;
                                    batch.slide += curln.height(panel.x);
                                }
                            }
                            batch.round = faux;
                            fresh_slide = batch.slide;
                        }
                    }
                    else
                    {
                        auto min_dy = std::min({ range1, range2, range3 });
                        if (min_dy > approx_threshold) // Calc approx.
                        {
                            ui64 count1 = std::min(std::max(0, fresh_slide), batch.vsize);
                            ui64 count2 = batch.vsize;
                            batch.ancid = front.index + (id_t)netxs::divround(batch.size * count1, count2);
                            batch.ancdy = 0;
                            batch.slide = fresh_slide;
                            batch.round = batch.vsize != batch.size;
                        }
                        else if (min_dy == range2 || fresh_slide <= 0) // Calc from the batch top.
                        {
                            if (delta2 <= 0) // Above 0.
                            {
                                batch.ancid = front.index;
                                batch.ancdy = delta2;
                            }
                            else if (delta2 > 0)
                            {
                                auto vpos = 0;
                                auto head = batch.begin();
                                auto tail = batch.end();
                                do
                                {
                                    auto& curln = *head;
                                    auto newpos = vpos + curln.height(panel.x);
                                    if (newpos > fresh_slide) break;
                                    else                      vpos = newpos;
                                }
                                while (++head != tail);
                                assert(vpos <= fresh_slide);
                                batch.ancid = head->index;
                                batch.ancdy = fresh_slide - vpos;
                            }
                            batch.slide = fresh_slide;
                            batch.round = faux;
                        }
                        else if (min_dy == range3 || fresh_slide >= batch.vsize) // Calc from the batch bottom.
                        {
                            if (delta3 <= 0) // Below batch.vsize.
                            {
                                batch.ancid = under.index;
                                batch.ancdy = under.height(panel.x) - delta3;
                            }
                            else if (delta3 > 0)
                            {
                                auto vpos = batch.vsize;
                                auto head = batch.begin();
                                auto tail = batch.end();
                                while (head != tail && vpos > fresh_slide)
                                {
                                    auto& curln = *--tail;
                                    vpos -= curln.height(panel.x);
                                }
                                assert(vpos <= fresh_slide);
                                batch.ancid = tail->index;
                                batch.ancdy = fresh_slide - vpos;
                            }
                            batch.slide = fresh_slide;
                            batch.round = faux;
                        }
                        else if (min_dy == range1) // Calc relative to ancid.
                        {
                            lookup();
                        }
                    }
                }

                return batch.slide == batch.basis;
            }
            // scroll_buf: Recalc batch.slide using anchoring by para_id + para_offset.
            void recalc_slide(bool away)
            {
                if (away)
                {
                    auto& front = batch.front();
                    auto& under = batch.back();
                    auto range1 = (si32)(under.index - batch.ancid);
                    auto range2 = (si32)(batch.ancid - front.index);
                    batch.round = faux;
                    if (range1 < batch.size)
                    {
                        if (approx_threshold < std::min(range1, range2))
                        {
                            auto& mapln = index.front();
                            auto c1 = (ui64)(si32)(mapln.index - front.index);
                            auto c2 = (ui64)range2;
                            auto fresh_slide = (si32)netxs::divround(batch.vsize * c2, c1);
                            batch.slide = batch.ancdy + fresh_slide;
                            batch.round = batch.vsize != batch.size;
                        }
                        else if (range1 < range2)
                        {
                            batch.slide = batch.ancdy + batch.vsize;
                            auto head = batch.end();
                            auto tail = head - (range1 + 1);
                            while (head != tail)
                            {
                                auto& curln = *--head;
                                batch.slide -= curln.height(panel.x);
                            }
                        }
                        else
                        {
                            batch.slide = batch.ancdy;
                            auto head = batch.begin();
                            auto tail = head + (range2 + 1);
                            while (head != tail)
                            {
                                auto& curln = *head++;
                                batch.slide += curln.height(panel.x);
                            }
                        }
                             if (batch.slide > batch.basis) batch.slide = batch.basis;
                        else if (batch.slide <= 0) // Overflow.
                        {
                            batch.ancid = front.index;
                            batch.ancdy = 0;
                            batch.slide = 0;
                            assert(batch.ancid == front.index);
                        }
                    }
                    else // Overflow.
                    {
                        batch.ancid = front.index;
                        batch.ancdy = 0;
                        batch.slide = 0;
                        assert(batch.ancid == front.index);
                    }
                }
                else
                {
                    auto& mapln = index.front();
                    batch.ancid = mapln.index;
                    batch.ancdy = mapln.start / panel.x;
                    batch.slide = batch.basis;
                    batch.round = faux;
                }
            }
            // scroll_buf: Resize viewport.
            void resize_viewport(twod new_sz, bool forced = faux) override
            {
                if (new_sz == panel && !forced) return;

                auto in_top = y_top - coord.y;
                auto in_end = coord.y - y_end;

                bufferbase::resize_viewport(new_sz);

                auto vsized = batch.check_size(panel);
                index.clear();

                // Preserve original content. The app that changed the margins is responsible for updating the content.
                auto upnew = std::max(upmin, twod{ panel.x, sctop });
                auto dnnew = std::max(dnmin, twod{ panel.x, scend });
                upbox.crop(upnew, brush.spare.dry());
                dnbox.crop(dnnew, brush.spare.dry());

                index.resize(arena); // Use a fixed ring because new lines are added much more often than a futures feed.
                auto away = batch.basis != batch.slide;

                auto& curln = batch.current();
                if (curln.wrapped() && batch.caret > curln.length()) // Dangling cursor.
                {
                    curln.crop(batch.caret, brush.spare.dry());
                    batch.recalc(curln);
                }

                if (!owner.bottom_anchored || in_top > 0 || in_end > 0) // The cursor is outside the scrolling region.
                {
                         if (in_top > 0) coord.y = std::max(0,           y_top - in_top);
                    else if (in_end > 0) coord.y = std::min(panel.y - 1, y_end + in_end);
                    coord = std::clamp(coord, dot_00, panel - dot_11);
                    if (owner.bottom_anchored)
                    {
                        batch.basis = std::max(0, batch.vsize - arena);
                    }
                    else // Try to keep batch.basis as is.
                    {
                        batch.basis = std::clamp(batch.basis, 0, std::max(0, batch.vsize - 1));
                    }
                    index_rebuild();
                    if (vsized || !away) recalc_slide(away);
                    return;
                }

                batch.basis = batch.vsize;
                auto lnid = curln.index;
                auto head = batch.end();
                auto maxn = batch.size - batch.index();
                auto tail = head - std::max(maxn, std::min(batch.size, arena));
                auto push = [&](auto i, auto o, auto r) { --batch.basis; index.push_front(i, o, r); };
                auto unknown = true;
                while (head != tail && (index.size < arena || unknown))
                {
                    auto& line = *--head;
                    auto lineid = line.index;
                    auto length = line.length();
                    auto active = lnid == lineid;
                    if (line.wrapped())
                    {
                        auto offset = length;
                        auto remain = length ? (length - 1) % panel.x + 1
                                             : 0;
                        do
                        {
                            offset -= remain;
                            push(lineid, offset, remain);
                            if (unknown && active && offset <= batch.caret)
                            {
                                auto eq = batch.caret && length == batch.caret;
                                unknown = faux;
                                coord.y = index.size;
                                coord.x = eq ? (batch.caret - 1) % panel.x + 1
                                             :  batch.caret      % panel.x;
                            }
                            remain = panel.x;
                        }
                        while (offset > 0 && (index.size < arena || unknown));
                    }
                    else
                    {
                        push(lineid, 0, length);
                        if (active)
                        {
                            unknown = faux;
                            coord.y = index.size;
                            coord.x = batch.caret;
                        }
                    }
                }
                coord.y = index.size - coord.y + y_top;
                if (vsized || !away) recalc_slide(away);

                assert(batch.basis >= 0);
                assert(test_futures());
                assert(test_coord());
                assert(test_resize());
            }
            // scroll_buf: Rebuild the next avail indexes from the known index (mapln).
            template<class Iter, class Index>
            void reindex(si32 avail, Iter curit, Index const& mapln)
            {
                auto& curln =*curit;
                auto  width = curln.length();
                auto  wraps = curln.wrapped();
                auto  curid = curln.index;
                auto  start = mapln.start + mapln.width;

                assert(curid == mapln.index);
                if (start == width) // Go to the next line.
                {
                    assert(curit != batch.end() - 1);

                    auto& line = *++curit;
                    width = line.length();
                    wraps = line.wrapped();
                    curid = line.index;
                    start = 0;
                }
                else assert(mapln.width == panel.x);

                assert(start % panel.x == 0);
                while (true)
                {
                    if (wraps)
                    {
                        auto trail = width - panel.x;
                        while (start < trail && avail-- > 0)
                        {
                            index.push_back(curid, start, panel.x);
                            start += panel.x;
                        }
                    }
                    if (avail-- <= 0) break;

                    assert(start == 0 || wraps);
                    index.push_back(curid, start, width - start);

                    if (avail == 0) break;

                    assert(curit != batch.end() - 1);
                    auto& line = *++curit;
                    width = line.length();
                    wraps = line.wrapped();
                    curid = line.index;
                    start = 0;
                }
                assert(test_index());
                assert(test_futures());
            }
            // scroll_buf: Rebuild index from the known index at y_pos.
            void index_rebuild_from(si32 y_pos)
            {
                assert(y_pos >= 0 && y_pos < index.size);

                auto& mapln = index[y_pos];
                auto  curit = batch.iter_by_id(mapln.index);
                auto  avail = std::min(batch.vsize - batch.basis, arena) - y_pos - 1;
                auto  drops = index.size - y_pos - 1;
                while (drops-- > 0) index.pop_back();

                if (avail > 0) reindex(avail, curit, mapln);
            }
            // scroll_buf: Rebuild index up to basis.
            void index_rebuild()
            {
                if (batch.basis >= batch.vsize)
                {
                    assert((log(prompt::term, "batch.basis >= batch.vsize  batch.basis=", batch.basis, " batch.vsize=", batch.vsize), true));
                    batch.basis = batch.vsize - 1;
                }

                index.clear();
                auto coor = batch.vsize;
                auto head = batch.end();
                while (coor != batch.basis)
                {
                    auto& curln = *--head;
                    auto  curid = curln.index;
                    auto length = curln.length();
                    if (curln.wrapped())
                    {
                        auto remain = length ? (length - 1) % panel.x + 1 : 0;
                        length -= remain;
                        index.push_front(curid, length, remain);
                        --coor;
                        while (length > 0 && coor != batch.basis)
                        {
                            length -= panel.x;
                            index.push_front(curid, length, panel.x);
                            --coor;
                        }
                    }
                    else
                    {
                        index.push_front(curid, 0, length);
                        --coor;
                    }
                }
                assert(test_futures());
            }
            // scroll_buf: Return scrollback height.
            si32 height() override
            {
                assert(test_height());
                return batch.vsize;
            }
            // scroll_buf: Recalc left and right oversize.
            bool recalc_pads(dent& oversz_ref) override
            {
                auto coor = get_coord();
                auto rght = std::max({0, batch.max<line::type::leftside>() - panel.x, coor.x - panel.x + 1 }); // Take into account the cursor position.
                auto left = std::max( 0, batch.max<line::type::rghtside>() - panel.x);
                auto cntr = std::max( 0, batch.max<line::type::centered>() - panel.x);
                auto bttm = std::max( 0, batch.vsize - batch.basis - arena          );
                auto both = cntr >> 1;
                left = shore + std::max(left, both + (cntr & 1));
                rght = shore + std::max(rght, both);
                if (oversz_ref.r != rght
                 || oversz_ref.l != left
                 || oversz_ref.b != bttm)
                {
                    oversz_ref.r = rght;
                    oversz_ref.l = left;
                    oversz_ref.b = bttm;
                    return true;
                }
                else return faux;
            }
            // scroll_buf: Check if there are futures, use them when scrolling regions.
            auto feed_futures(si32 query)
            {
                assert(test_futures());
                assert(test_coord());
                assert(query > 0);

                auto stash = batch.vsize - batch.basis - index.size;
                auto avail = si32{};
                if (stash > 0)
                {
                    avail = std::min(stash, query);
                    batch.basis += avail;

                    auto& mapln = index.back();
                    auto  curit = batch.iter_by_id(mapln.index);

                    reindex(avail, curit, mapln);
                }
                else avail = 0;

                return avail;
            }
            // scroll_buf: Return current 0-based cursor position in the scrollback.
            twod get_coord(twod origin = {}) override
            {
                auto coor = coord;
                if (coor.y >= y_top
                 && coor.y <= y_end)
                {
                    coor.y += batch.basis;

                    auto visible = coor.y + origin.y;
                    if (visible < y_top // Do not show cursor behind margins when the scroll region is dragged by mouse.
                     || visible > y_end)
                    {
                        coor.y = dot_mx.y;
                    }

                    auto& curln = batch.current();
                    auto  align = curln.style.jet();

                    if (align == bias::left
                     || align == bias::none) return coor;

                    auto curidx = coord.y - y_top;
                    auto remain = index[curidx].width;
                    if (remain == panel.x && curln.wrapped()) return coor;

                    if    (align == bias::right )  coor.x += panel.x     - remain - 1;
                    else /*align == bias::center*/ coor.x += panel.x / 2 - remain / 2;
                }
                else
                {
                    coor -= origin;
                }
                return coor;
            }
            // scroll_buf: Set cursor position and sync it with buffer.
            void set_coord(twod new_coord) override
            {
                bufferbase::set_coord(new_coord);
                sync_coord();
            }
            // scroll_buf: Map the current cursor position to the scrollback.
            template<bool AllowPendingWrap = true>
            void sync_coord()
            {
                coord.y = std::clamp(coord.y, 0, panel.y - 1);
                if (coord.x < 0) coord.x = 0;

                if (coord.y >= y_top && coord.y <= y_end)
                {
                    auto& curln = batch.current();
                    auto  wraps = curln.wrapped();
                    auto  curid = curln.index;
                    if constexpr (AllowPendingWrap)
                    {
                        if (coord.x > panel.x && wraps) coord.x = panel.x;
                    }
                    else
                    {
                        if (coord.x >= panel.x && wraps) coord.x = panel.x - 1;
                    }

                    coord.y -= y_top;

                    if (index.size <= coord.y)
                    {
                        auto add_count = coord.y - (index.size - 1);
                        add_lines(add_count);
                    }

                    auto& mapln = index[coord.y];
                    batch.caret = mapln.start + coord.x;

                    if (curid != mapln.index)
                    {
                        auto newix = batch.index_by_id(mapln.index);
                        batch.index(newix);

                        if (batch->style != parser::style)
                        {
                            _set_style(parser::style);
                            assert(newix == batch.index_by_id(index[coord.y].index));
                        }
                    }
                    coord.y += y_top;

                    assert((batch.caret - coord.x) % panel.x == 0);
                }
                else // Always wraps inside margins.
                {
                    if constexpr (AllowPendingWrap)
                    {
                        if (coord.x > panel.x) coord.x = panel.x;
                    }
                    else
                    {
                        if (coord.x >= panel.x) coord.x = panel.x - 1;
                    }
                }
            }

            void  cup(fifo& q) override { bufferbase:: cup(q); sync_coord<faux>(); }
            void  cup(twod  p) override { bufferbase:: cup(p); sync_coord<faux>(); }
            void cup0(twod  p) override { bufferbase::cup0(p); sync_coord<faux>(); }
            void cup2(twod  p) override { bufferbase::cup2(p); sync_coord<faux>(); }
            void  cuf(si32  n) override { bufferbase:: cuf(n); sync_coord<faux>(); }
            void  cub(si32  n) override { bufferbase:: cub(n); sync_coord<faux>(); }
            void _cub(si32  n) override { bufferbase::_cub(n); batch.caret -= n;   }
            void  chx(si32  n) override { bufferbase:: chx(n); sync_coord<faux>(); }
            void chx0(si32  n) override { bufferbase::chx0(n); sync_coord<faux>(); }
            void  tab(si32  n) override { bufferbase:: tab(n); sync_coord<faux>(); }
            void  chy(si32  n) override { bufferbase:: chy(n); sync_coord(); }
            void chy0(si32  n) override { bufferbase::chy0(n); sync_coord(); }
            void  scl(si32  n) override { bufferbase:: scl(n); sync_coord(); }
            void   il(si32  n) override { bufferbase::  il(n); sync_coord(); }
            void   dl(si32  n) override { bufferbase::  dl(n); sync_coord(); }
            void   up(si32  n) override { bufferbase::  up(n); sync_coord(); }
            void   dn(si32  n) override { bufferbase::  dn(n); sync_coord(); }
            void   lf(si32  n) override { bufferbase::  lf(n); sync_coord(); }
            void break_soft_wrap_before_hard_lf()
            {
                if (coord.y < y_top || coord.y > y_end) return;
                if (panel.x <= 0 || arena <= 0) return;

                auto row = coord.y - y_top;
                if (row < 0 || row >= arena) return;

                auto split = row + 1;
                if (split > arena) return;

                auto& mapln = index[row];
                auto& curln = batch.item_by_id(mapln.index);
                if (!curln.wrapped()) return;

                auto line_height = curln.height(panel.x);
                if (mapln.start / panel.x + 1 >= line_height) return;

                dissect(split);
            }
            void hard_lf(si32 n, bool with_cr = faux) override
            {
                parser::flush_data();
                if (with_cr) cr();
                if (n <= 0)
                {
                    bufferbase::_lf(n);
                    sync_coord();
                    return;
                }
                while (n-- > 0)
                {
                    // break_soft_wrap_before_hard_lf();
                    bufferbase::_lf(1);
                    sync_coord();
                }
            }
            void  _lf(si32  n) override { bufferbase:: _lf(n); sync_coord(); }
            void  _ri(si32  n) override { bufferbase:: _ri(n); sync_coord(); }
            void   ri()        override { bufferbase::  ri();  sync_coord(); }
            void   cr()        override { bufferbase::  cr();  sync_coord(); }

            // scroll_buf: Reset the scrolling region.
            void reset_scroll_region()
            {
                upmin = dot_00;
                dnmin = dot_00;
                upbox.crop(upmin);
                dnbox.crop(dnmin);
                arena = panel.y;
                index.resize(arena);
                index_rebuild();
                bufferbase::set_scroll_region(0, 0);
                sync_coord();
            }
            // scroll_buf: Set the scrolling region using 1-based top and bottom. Use 0 to reset.
            void set_scroll_region(si32 top, si32 bottom) override
            {
                auto old_sctop = sctop;
                auto old_scend = scend;

                bufferbase::set_scroll_region(top, bottom); // coord -> dot_00 -- coord is unsynced
                if (old_sctop == sctop && old_scend == scend)
                {
                    sync_coord();
                    return;
                }

                // Trim the existing margin content if any. The app that changed the margins is responsible for updating the content.
                upmin = { panel.x, sctop };
                dnmin = { panel.x, scend };
                auto delta_top = sctop - old_sctop;
                auto delta_end = scend - old_scend;

                // Take lines from the scrollback.
                auto pull = [&](face& block, twod origin, si32 begin, si32 limit)
                {
                    if (begin >= index.size) return;
                    limit = std::min(limit, index.size);
                    dissect(begin);
                    dissect(limit);
                    auto from = index[begin    ].index;
                    auto upto = index[limit - 1].index + 1;
                    auto base = batch.index_by_id(from);
                    auto head = batch.begin() + base;
                    auto size = (si32)(upto - from);
                    auto tail = head + size;
                    auto area = block.area();
                    block.full(area);
                    block.clip(area);
                    block.ac(origin);
                    do
                    {
                        auto& curln = *head;
                        block.output(curln, cell::shaders::fuse);
                        block.nl(1);
                    }
                    while (++head != tail);
                    batch.remove(base, size);
                };
                // Return lines to the scrollback iif margin is disabled.
                auto push = [&](face& block, bool at_bottom)
                {
                    auto size = block.size();
                    if (size.y <= 0) return;

                    auto start = si32{};
                    if (at_bottom)
                    {
                        auto stash = batch.vsize - batch.basis - arena;
                        if (stash > 0)
                        {
                            dissect(arena);
                            start = batch.index_by_id(index.back().index) + 1;
                        }
                        else // Add new lines if needed.
                        {
                            auto count = arena - index.size;
                            auto curid = batch.back().index;
                            while (count-- > 0) batch.invite(++curid, parser::style, parser::brush);
                            start = batch.size;
                        }
                    }
                    else
                    {
                        dissect(0);
                        start = batch.index_by_id(index.front().index);
                    }

                    auto curit = block.begin();
                    auto width = twod{ size.x, 1 };
                    auto curid = start == 0 ? batch.front().index
                                            : batch[start - 1].index + 1;
                    auto style = ansi::def_style;
                    style.wrp(wrap::off);
                    while (size.y-- > 0)
                    {
                        auto oldsz = batch.size;
                        auto proto = core::span{ curit, (size_t)size.x };
                        auto curln = line{ curid++, style, proto, width };
                        curln.shrink(block.mark());
                        batch.insert(start, std::move(curln));
                        start += batch.size - oldsz; // Due to circulation in the ring.
                        assert(start <= batch.size);
                        curit += size.x;
                    }
                    batch.reindex(start);
                };

                if (delta_end > 0)
                {
                    if (old_scend == 0) dnbox.mark(brush.spare);
                    dnbox.crop<true>(dnmin);
                    pull(dnbox, dot_00, arena - delta_end, arena);
                }
                else
                {
                    if (scend == 0 && old_scend > 0) push(dnbox, true);
                    dnbox.crop<true>(dnmin);
                }

                if (delta_top > 0)
                {
                    if (old_sctop == 0) upbox.mark(brush.spare);
                    upbox.crop<faux>(upmin);
                    pull(upbox, { 0, old_sctop }, 0, delta_top);
                    if (batch.size == 0) batch.invite(0, parser::style, parser::brush);
                }
                else
                {
                    if (sctop == 0 && old_sctop > 0) push(upbox, faux);
                    upbox.crop<faux>(upmin);
                }

                arena = panel.y - (scend + sctop);
                index.clear();
                index.resize(arena);
                index_rebuild();
                sync_coord();
            }
            // scroll_buf: Push empty lines to the scrollback bottom.
            void add_lines(si32 amount)
            {
                assert(amount >= 0);
                auto line_id = batch.back().index;
                while (amount-- > 0)
                {
                    batch.invite(++line_id, parser::style, parser::brush);
                    index.push_back(line_id, 0, 0);
                }
            }
            // scroll_buf: Push filled lines to the scrollback bottom.
            void add_lines(si32 amount, cell const& blank)
            {
                assert(amount >= 0);
                auto line_id = batch.back().index;
                while (amount-- > 0)
                {
                    batch.invite(++line_id, parser::style, blank, panel.x);
                    index.push_back(line_id, 0, panel.x);
                }
            }
            // scroll_buf: .
            template<feed Dir>
            auto xconv(si32 x, bias align, si32 remain) const
            {
                // forward: screen -> offset
                // reverse: offset -> screen
                auto map = [](auto& a, auto b)
                {
                    Dir == feed::fwd ? a -= b
                                     : a += b;
                };
                switch (align)
                {
                    case bias::none:
                    case bias::left:   break;
                    case bias::right:  map(x, panel.x     - remain    ); break;
                    case bias::center: map(x, panel.x / 2 - remain / 2); break;
                };
                return x;
            }
            // scroll_buf: .
            auto screen_to_offset(line const& curln, twod coor) const
            {
                auto length = curln.length();
                auto adjust = curln.style.jet();
                if (curln.wrapped() && length > panel.x)
                {
                    auto endpos = length - 1;
                    auto height = endpos / panel.x;
                    auto remain = endpos % panel.x + 1;
                    coor.x = coor.y < height ? std::clamp(coor.x, 0, panel.x - 1)
                                             : std::clamp(xconv<feed::fwd>(coor.x, adjust, remain), 0, remain - 1);
                    coor.x+= coor.y * panel.x;
                }
                else
                {
                    coor.x = std::clamp(xconv<feed::fwd>(coor.x, adjust, length), 0, length ? length - 1 : 0);
                }
                return coor.x;
            }
            // scroll_buf: .
            auto offset_to_screen(line const& curln, si32 offset) const
            {
                auto size = curln.length();
                auto last = size ? size - 1 : 0;
                auto coor = twod{ std::clamp(offset, 0, last), 0 };
                if (size > 1 && curln.wrapped())
                {
                    coor.y = coor.x / panel.x;
                    coor.x = coor.x % panel.x;
                    if (coor.y < last / panel.x) return coor;
                    size = last % panel.x + 1;
                }
                coor.x = xconv<feed::rev>(coor.x, curln.style.jet(), size);
                return coor;
            }
            void paint_tail_fill(face& dest, line const& curln, twod coor)
            {
                if (!curln.fills_eol()) return;

                auto start = std::clamp(curln.fill_from(), 0, panel.x);
                if (start >= panel.x) return;

                auto width = curln.length();
                auto from = start;
                if (auto span = std::clamp(std::max(width, start), 0, panel.x); span > 0)
                {
                    from = xconv<feed::rev>(start, curln.style.jet(), span);
                }

                if (from >= panel.x) return;

                auto area = rect{{ coor.x + from, coor.y }, { panel.x - from, 1 }};
                dest.fill(area, cell::shaders::full(curln.fill_cell()));
            }
            template<class Shader>
            void output_line(face& dest, line const& curln, twod coor, Shader shader)
            {
                paint_tail_fill(dest, curln, coor);
                dest.output(curln, coor, shader);
            }
            void output_line(face& dest, line const& curln, twod coor)
            {
                paint_tail_fill(dest, curln, coor);
                dest.output(curln, coor);
            }
            void set_tail_fill(line& curln, si32 start, cell const& blank)
            {
                start = std::clamp(start, 0, panel.x);
                if (start >= panel.x) curln.reset_fill();
                else                  curln.fill_to_eol(start, blank);
            }
            void advance_tail_fill(line& curln, si32 start, si32 count)
            {
                if (!curln.fills_eol()) return;

                auto from = curln.fill_from();
                auto tail = curln.fill_cell();
                auto end = std::clamp(start + count, 0, panel.x);
                if (start > from)
                {
                    if (curln.length() < from)
                    {
                        curln.reset_fill();
                        return;
                    }
                    curln.crop(start, tail);
                }
                if (end > from)
                {
                    set_tail_fill(curln, end, tail);
                }
            }
            void isolate_current_visual_row()
            {
                if (coord.y < y_top || coord.y > y_end) return;
                if (panel.x <= 0 || arena <= 0) return;

                auto row = coord.y - y_top;
                if (row < 0 || row >= arena) return;

                auto& mapln = index[row];
                if (mapln.start != 0)
                {
                    dissect(row);
                }

                auto& current = index[row];
                auto& curln = batch.item_by_id(current.index);
                if (curln.wrapped() && curln.height(panel.x) > 1 && row + 1 <= arena)
                {
                    dissect(row + 1);
                }
                sync_coord();
            }
            // scroll_buf: Snap linear selection edges to complete grapheme boundaries.
            void normalize_line_edge(line const& curln, twod& edge, bool start_edge) const
            {
                auto length = curln.length();
                if (length <= 0) return;

                auto offset = screen_to_offset(curln, edge);
                offset = std::clamp(offset, 0, length - 1);
                auto const& c = curln.at(offset);
                auto [w, h, x, y] = c.whxy();
                auto adjusted = faux;
                if (h == 1 && w > 1)
                {
                    if (start_edge && x > 1)
                    {
                        offset -= x - 1;
                        adjusted = true;
                    }
                    else if (!start_edge && x < w)
                    {
                        offset += w - x;
                        adjusted = true;
                    }
                    if (adjusted)
                    {
                        offset = std::clamp(offset, 0, length - 1);
                        edge = offset_to_screen(curln, offset);
                    }
                }
            }
            // scroll_buf: Snap linear selection edges to complete grapheme boundaries.
            void normalize_canvas_edges(rich const& board, twod& edge_1, twod& edge_2) const
            {
                auto size = board.size();
                if (size.x <= 0 || size.y <= 0) return;
                auto limits = size - dot_11;
                edge_1 = std::clamp(edge_1, dot_00, limits);
                edge_2 = std::clamp(edge_2, dot_00, limits);

                auto to_offset = [&](auto p)
                {
                    return p.x + p.y * size.x;
                };
                auto to_coord = [&](auto offset)
                {
                    return twod{ offset % size.x, offset / size.x };
                };
                auto cells = size.x * size.y;
                auto head = to_offset(edge_1);
                auto tail = to_offset(edge_2);
                auto swap = head > tail;
                if (swap) std::swap(head, tail);

                auto normalize = [&](auto& offset, auto start_edge)
                {
                    offset = std::clamp(offset, 0, cells - 1);
                    auto const& c = *(board.begin() + offset);
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
            // scroll_buf: Update current SGR attributes. (! Check coord.y context)
            void _set_style(deco const& new_style)
            {
                auto& curln = batch.current();
                auto  wraps = curln.wrapped();
                auto  width = curln.length();
                curln.style = new_style;

                if (batch.caret > width) // Dangling cursor.
                {
                    if (!wraps || coord.x <= panel.x) // Extend the line if the cursor is inside the viewport.
                    {
                        width = batch.caret;
                        curln.crop(width, brush.spare.dry());
                    }
                    else // Move coord.x inside viewport for wrapped lines (cursor came from another (unwrapped) line).
                    {
                        auto& mapln = index[coord.y];
                        batch.caret = mapln.start + panel.x;
                        mapln.width = panel.x;
                        coord.x = panel.x;
                        curln.crop(batch.caret, brush.spare.dry());
                    }
                }

                batch.recalc(curln);

                if (wraps != curln.wrapped())
                {
                    if (batch.caret >= panel.x)
                    {
                        if (wraps)
                        {
                            if (coord.x == 0) coord.y -= 1;
                            coord.x  =  batch.caret;
                            coord.y -= (batch.caret - 1) / panel.x;
                            if (coord.y < 0)
                            {
                                batch.basis -= std::abs(coord.y);
                                assert(batch.basis >= 0);
                                coord.y = 0;
                            }
                        }
                        else
                        {
                            if (batch.caret == width)
                            {
                                coord.x  = (batch.caret - 1) % panel.x + 1;
                                coord.y += (batch.caret - 1) / panel.x;
                            }
                            else
                            {
                                coord.x  = batch.caret % panel.x;
                                coord.y += batch.caret / panel.x;
                            }

                            if (coord.y >= arena)
                            {
                                auto limit = arena - 1;
                                auto delta = coord.y - limit;
                                batch.basis += delta;
                                coord.y = limit;
                            }
                        }
                        index_rebuild();
                    }
                    else
                    {
                        auto& mapln = index[coord.y];
                        wraps = curln.wrapped();
                        mapln.start = 0;
                        mapln.width = wraps ? std::min(panel.x, width)
                                            : width;
                        index_rebuild_from(coord.y);
                    }
                }
                assert(test_index());
            }
            // scroll_buf: Proceed style update (parser callback).
            void meta(deco const& old_style) override
            {
                if (batch->style != parser::style)
                {
                    if (coord.y >= y_top
                     && coord.y <= y_end)
                    {
                        coord.y -= y_top;
                        _set_style(parser::style);
                        coord.y += y_top;
                    }
                }
                bufferbase::meta(old_style);
            }
            [[ nodiscard ]]
            auto get_context(twod& pos)
            {
                struct qt
                {
                    twod& c;
                    si32  t;
                    bool  b;
                    rich& block;
                    qt(twod& cy, si32 ct, bool cb, scroll_buf& cs)
                        : c{ cy },
                          t{ ct },
                          b{ cb },
                      block{ c.y > cs.y_end ? (void)(t = cs.y_end + 1), cs.dnbox
                                            :                           cs.upbox }
                         { c.y -= t; }
                   ~qt() { c.y += t; }
                    operator bool () { return b; }
                };
                auto inside = coord.y >= y_top
                           && coord.y <= y_end;
                return qt{ pos, inside ? y_top : 0, inside, *this };
            }
            // scroll_buf: CSI n K  Erase line (don't move cursor).
            void el(si32 n) override
            {
                bufferbase::flush();
                //todo revise - nul() or dry()
                //auto blank = brush.dry();
                auto blank = brush.spc(); // ok
                // Note: isolate_current_visual_row() was removed here.
                // For wrapped lines on non-last visual rows, splice is used
                // instead of crop to avoid splitting the logical line.
                if (auto ctx = get_context(coord))
                {
                    auto  start = si32{};
                    auto  count = si32{};
                    auto cursor = std::max(0, batch.caret);
                    auto& curln = batch.current();
                    auto  width = curln.length();
                    auto  wraps = curln.wrapped();
                    switch (n)
                    {
                        default:
                        case commands::erase::line::right: // n = 0 (default)  Erase to Right.
                            start = cursor;
                            count = wraps ? coord.x == panel.x ? 0 : panel.x - (cursor + panel.x) % panel.x
                                          : std::max(0, std::max(panel.x, width) - cursor);
                            break;
                        case commands::erase::line::left: // n = 1  Erase to Left.
                            start = wraps ? cursor - cursor % panel.x
                                          : 0;
                            count = cursor - start + 1; // +1 to include the current cell.
                            break;
                        case commands::erase::line::all: // n = 2  Erase All.
                            start = wraps ? cursor - cursor % panel.x
                                          : 0;
                            count = wraps ? panel.x
                                          : std::max(panel.x, batch->length());
                            break;
                        case commands::erase::line::wraps: // n = 3  Erase wrapped line.
                            start = cursor;
                            count = width - cursor;
                            break;
                    }
                    if (count)
                    {
                        if (n == 1) // Erase to Left.
                        {
                            curln.splice<faux>(start, count, blank);
                            if (curln.fills_eol() && start + count > curln.fill_from())
                            {
                                auto tail = curln.fill_cell();
                                set_tail_fill(curln, start + count, tail);
                            }
                            batch.recalc(curln);
                            width = curln.length();
                            auto& mapln = index[coord.y];
                            mapln.width = wraps ? std::min(panel.x, width - mapln.start)
                                                : width;
                        }
                        else if (n == commands::erase::line::wraps)
                        {
                            curln.crop(start);
                            curln.reset_fill();
                            batch.recalc(curln);
                            index_rebuild();
                        }
                        else
                        {
                            curln.crop(std::min(start, curln.length()));
                            set_tail_fill(curln, start, blank);
                            batch.recalc(curln);
                            index_rebuild();
                        }
                    }
                }
                else alt_screen::_el(n, ctx.block, coord, panel, blank);
            }
            // scroll_buf: CSI n @  ICH. Insert n colored blanks after cursor. Existing chars after cursor shifts to the right. Don't change cursor pos.
            void ins(si32 n) override
            {
                bufferbase::flush();
                auto blank = brush.spc(); // ok
                if (auto ctx = get_context(coord))
                {
                    n = std::min(n, panel.x - coord.x);
                    auto& curln = batch.current();
                    curln.reset_fill();
                    curln.insert(batch.caret, n, blank, panel.x);
                    batch.recalc(curln); // Line front is filled by blanks. No wrapping.
                    auto  width = curln.length();
                    auto  wraps = curln.wrapped();
                    auto& mapln = index[coord.y];
                    mapln.width = wraps ? std::min(panel.x, width - mapln.start)
                                        : width;
                }
                else ctx.block.insert(coord, n, blank);
            }
            // scroll_buf: CSI n P  Delete (not Erase) letters under the cursor. Line end is filled by defclr. Length is preserved. No wrapping.
            void dch(si32 n) override
            {
                bufferbase::flush();
                auto blank = brush.spare.spc(); // ok
                if (auto ctx = get_context(coord))
                {
                    auto& curln = batch.current();
                    curln.reset_fill();
                    auto old_len = curln.length();
                    curln.cutoff(batch.caret, n, blank, panel.x);
                    curln.shrink(blank, 0, std::max(0, old_len - n));
                    batch.recalc(curln);
                    index_rebuild();
                }
                else ctx.block.cutoff(coord, n, blank);
            }
            // scroll_buf: Move internal caret by count with wrapping.
            void _fwd(si32 count)
            {
                coord.x += count;
                if (count > 0)
                {
                    if (coord.y < y_top)
                    {
                        if (coord.x > panel.x)
                        {
                            wrapdn();
                            if (coord.y >= y_top)
                            {
                                count -= coord.x + (coord.y - y_top) * panel.x;
                                set_coord(twod{ 0, y_top });
                                _fwd(count);
                            }
                        }
                    }
                    else if (coord.y <= y_end)
                    {
                        auto& curln = batch.current();
                        if (coord.x >= panel.x && curln.wrapped())
                        {
                            wrapdn();
                            if (coord.y > y_end)
                            {
                                batch.basis += coord.y - y_end;
                                coord.y = y_end;
                                index_rebuild();
                            }
                        }
                    }
                    else
                    {
                        if (coord.x > panel.x)
                        {
                            wrapdn();
                            if (coord.y >= panel.y) coord = panel - dot_01;
                        }
                    }
                }
                else if (count < 0)
                {
                    if (coord.y < y_top)
                    {
                        if (coord.x < 0)
                        {
                            wrapup();
                            if (coord.y < 0)
                            {
                                coord.y += coord.x / panel.x;
                                coord.x  = coord.x % panel.x;
                                if (coord.y < 0) coord = dot_00;
                            }
                        }
                    }
                    else if (coord.y <= y_end)
                    {
                        auto& curln = batch.current();
                        if (coord.x < 0 && curln.wrapped())
                        {
                            wrapup(); //failed for CUF(-(panel.x + 1)) at coord.x = 1
                            if (coord.y < y_top)
                            {
                                coord.y += coord.x / panel.x;
                                coord.x  = coord.x % panel.x;
                                if (coord.y < y_top)
                                {
                                    batch.basis -= y_top - coord.y;
                                    if (batch.basis < 0) // Scroll down by pushing -batch.basis empty lines to front.
                                    {
                                        auto n = -batch.basis;
                                        while (n-- > 0) batch.insert(0, id_t{}, parser::style, parser::brush);
                                        batch.reindex(batch.basis); // Reindex backward.
                                        batch.basis = 0;
                                    }
                                    coord.y = y_top;
                                    index_rebuild();
                                }
                            }
                        }
                    }
                    else
                    {
                        if (coord.x < 0)
                        {
                            wrapup();
                            if (coord.y <= y_end)
                            {
                                count += coord.x + (coord.y - y_end) * panel.x;
                                set_coord(twod{ panel.x, y_end });
                                _fwd(count);
                            }
                        }
                    }
                }

                if (count) sync_coord();
                auto& curln = batch.current();
                if (coord.x == panel.x && curln.wrapped() && batch.caret < curln.length())
                {
                    coord.x = 0;
                    coord.y++;
                    if (coord.y > y_end)
                    {
                        batch.basis += coord.y - y_end;
                        coord.y = y_end;
                        index_rebuild();
                    }
                }
                assert(test_coord());
            }
            // scroll_buf: '\x7F'  Delete letters backward (by defclr) and move cursor back. Nobody do it (tested in WT, VTE).
            void del(si32 n) override
            {
                bufferbase::flush();
                n = std::min(n, batch.caret);
                if (batch.caret > 0 && n > 0)
                {
                    _fwd(-n);
                    auto& curln = batch.current();
                    curln.splice<faux>(batch.caret, n, brush.spare.spc());
                    batch.recalc(curln);
                }
            }
            // scroll_buf: Move cursor by n in line.
            void move(si32 n) override
            {
                bufferbase::flush();
                _fwd(n);
            }
            // scroll_buf: CSI n X  Erase/put n chars after cursor. Don't change cursor pos.
            void ech(si32 n, char c = '\0') override
            {
                parser::flush();
                auto blank = brush;
                blank.txt(c);
                if (auto ctx = get_context(coord))
                {
                    n = std::min(n, panel.x - coord.x);
                    auto& curln = batch.current();
                    curln.reset_fill();
                    //todo revise (brush != default ? see windows console)
                    //if (c == whitespace) curln.splice<faux>(batch.caret, n, blank);
                    //else                 curln.splice<true>(batch.caret, n, blank);

                    if ((c == '\0' || c == ' ') && batch.caret + n >= curln.length())
                    {
                        curln.crop(batch.caret, blank);
                        batch.recalc(curln);
                        index_rebuild();
                    }
                    else
                    {
                        curln.splice<false>(batch.caret, n, blank);
                        batch.recalc(curln);
                        auto& mapln = index[coord.y];
                        auto  width = curln.length();
                        auto  wraps = curln.wrapped();
                        mapln.width = wraps ? std::min(panel.x, width - mapln.start)
                                            : width;
                    }
                }
                else ctx.block.splice(coord, n, blank);
            }
            // scroll_buf: Merge curln with its neighbors.
            void _merge(line& curln, si32 oldsz, ui32 curid, si32 count)
            {
                auto coor = oldsz + panel.x - (oldsz - 1) % panel.x - 1;
                auto iter = batch.iter_by_id(curid);
                while (count-- > 0)
                {
                    auto& line = *++iter;
                    //todo respect line alignment
                    if (line.wrapped()) curln.splice(coor, line                   , cell::shaders::full, brush.spc());
                    else                curln.splice(coor, line.substr(0, panel.x), cell::shaders::full, brush.spc());
                    coor += line.height(panel.x) * panel.x;
                }
            }
            // scroll_buf: Proceed new text using specified cell shader.
            template<bool Copy = faux, class Span, class Shader>
            void _data(si32 count, Span const& proto, Shader fuse)
            {
                static constexpr auto mixer = !std::is_same_v<Shader, decltype(cell::shaders::full)>;

                assert(coord.y >= 0 && coord.y < panel.y);
                assert(test_futures());
                assert(test_coord());

                if (coord.y < y_top)
                {
                    auto start = coord;
                    coord.x += count;
                    //todo apply line adjusting (necessity is not clear)
                    if (coord.x <= panel.x)//todo styles! || ! curln.wrapped())
                    {
                        auto n = std::min(count, panel.x - std::max(0, start.x));
                        upbox.splice<Copy>(start, n, proto, fuse);
                    }
                    else
                    {
                        wrapdn();
                        if (coord.y >= y_top)
                        {
                            auto n = coord.x + (coord.y - y_top) * panel.x;
                            count -= n;
                            set_coord(twod{ 0, y_top });
                            _data<Copy>(n, proto, fuse); // Reversed fill using the last part of the proto.
                        }
                        auto data = proto.begin();
                        auto seek = start.x + start.y * panel.x;
                        auto dest = upbox.begin() + seek;
                        auto tail = dest + count;
                        rich::forward_fill_proc<Copy>(data, dest, tail, fuse);
                    }
                    // Note: coord can be unsync due to scroll regions.
                }
                else if (coord.y <= y_end)
                {
                    coord.y -= y_top;
                    auto& curln = batch.current();
                    auto  start = batch.caret;
                    advance_tail_fill(curln, start, count);
                    batch.caret += count;
                    coord.x     += count;
                    if (batch.caret <= panel.x || !curln.wrapped()) // case 0.
                    {
                        curln.splice<Copy>(start, count, proto, fuse, brush.spare.spc());
                        auto& mapln = index[coord.y];
                        assert(coord.x % panel.x == batch.caret % panel.x && mapln.index == curln.index);
                        if (coord.x > mapln.width)
                        {
                            mapln.width = coord.x;
                            batch.recalc(curln);
                        }
                        else assert(curln._size == curln.length());
                    } // case 0 - done.
                    else
                    {
                        auto max_y = arena - 1;
                        auto cur_y = coord.y + batch.basis;
                        wrapdn();

                        auto query = coord.y - (index.size - 1);
                        if (query > 0)
                        {
                            auto avail = feed_futures(query);
                            query   -= avail;
                            coord.y -= avail;
                        }

                        auto oldsz = curln.length();
                        auto curid = curln.index;
                        if (query > 0) // case 3 - complex: Cursor is outside the viewport.
                        {              // cursor overlaps some lines below and placed below the viewport.
                            curln.resize(batch.caret, brush.spare.spc());
                            batch.recalc(curln);
                            if (auto n = (si32)(batch.back().index - curid))
                            {
                                if constexpr (mixer) _merge(curln, oldsz, curid, n);
                                assert(n > 0);
                                while (n-- > 0) batch.pop_back();
                            }

                            auto w = curln.length();
                            auto a = panel.x;
                            auto b = w - a;

                            cur_y -= batch.basis;
                            if (cur_y > 0)
                            {
                                auto n = index.size - cur_y - 1;
                                while (n-- > 0) index.pop_back();
                                auto& mapln = index.back();
                                mapln.width = panel.x;
                                a += mapln.start;
                            }
                            else // cur_y has scrolled out.
                            {
                                index.clear();
                                a *= std::abs(cur_y);
                            }

                            while (a < b)
                            {
                                index.push_back(curid, a, panel.x);
                                a += panel.x;
                            }
                            index.push_back(curid, a, w - a);

                            if (coord.y > max_y)
                            {
                                batch.basis += coord.y - max_y;
                                coord.y = max_y;
                            }

                            assert(test_futures());
                        } // case 3 done
                        else
                        {
                            auto& mapln = index[coord.y];
                            if (curid == mapln.index) // case 1 - plain: cursor is inside the current paragraph.
                            {
                                curln.resize(batch.caret, brush.spare.spc());
                                if (batch.caret - coord.x == mapln.start)
                                {
                                    if (coord.x > mapln.width)
                                    {
                                        mapln.width = coord.x;
                                        batch.recalc(curln);
                                    }
                                    else assert(curln._size == curln.length());
                                }
                                else // The case when the current line completely fills the viewport (arena == 1).
                                {
                                    assert(arena == 1);
                                    mapln.start = batch.caret - coord.x;
                                    mapln.width = coord.x;
                                    batch.recalc(curln);
                                }
                                assert(test_futures());
                            } // case 1 done.
                            else // case 2 - fusion: cursor overlaps lines below but stays inside the viewport.
                            {
                                auto& destln = batch.item_by_id(mapln.index);
                                //todo respect destln alignment
                                auto  shadow = destln.wrapped() ? destln.substr(mapln.start + coord.x)
                                                                : destln.substr(mapln.start + coord.x, std::min(panel.x, mapln.width) - coord.x);

                                if constexpr (mixer) curln.resize(batch.caret +shadow.length(), brush.spare.spc());
                                else                 curln.splice(batch.caret, shadow, cell::shaders::full, brush.spare.spc());

                                batch.recalc(curln);
                                auto w = curln.length();
                                auto spoil = (si32)(mapln.index - curid);
                                assert(spoil > 0);

                                if constexpr (mixer) _merge(curln, oldsz, curid, spoil);

                                auto after = batch.index() + 1;
                                spoil = batch.remove(after, spoil);

                                if (cur_y < batch.basis)
                                {
                                    index_rebuild(); // Update index. (processing lines larger than viewport)
                                }
                                else
                                {
                                    cur_y -= batch.basis;
                                    auto idx_a = index.begin() + cur_y;
                                    auto idx_b = index.end();
                                    auto a = idx_a->start;
                                    auto b = w - panel.x;
                                    while (idx_a != idx_b && a < b) // Update for current line.
                                    {
                                        auto& i =*idx_a;
                                        i.index = curid;
                                        i.start = a;
                                        i.width = panel.x;
                                        a += panel.x;
                                        ++idx_a;
                                    }
                                    if (idx_a != idx_b)
                                    {
                                        auto& i = *idx_a;
                                        i.index = curid;
                                        i.start = a;
                                        i.width = w - a;
                                        ++idx_a;
                                        while (idx_a != idx_b) // Update the rest.
                                        {
                                            auto& j = *idx_a;
                                            j.index -= spoil;
                                            ++idx_a;
                                        }
                                    }
                                    assert(test_index());
                                }
                                assert(test_futures());
                            } // case 2 done.
                        }
                        batch.current().splice<Copy>(start, count, proto, fuse, brush.spare.spc());
                    }
                    assert(coord.y >= 0 && coord.y < arena);
                    coord.y += y_top;
                }
                else
                {
                    coord.y -= y_end + 1;
                    auto start = coord;
                    coord.x += count;
                    //todo apply line adjusting
                    if (coord.x <= panel.x)//todo styles! || ! curln.wrapped())
                    {
                        auto n = std::min(count, panel.x - std::max(0, start.x));
                        dnbox.splice<Copy>(start, n, proto, fuse);
                    }
                    else
                    {
                        wrapdn();
                        auto data = proto.begin();
                        auto size = count;
                        auto seek = start.x + start.y * panel.x;
                        auto dest = dnbox.begin() + seek;
                        auto tail = dnbox.end();
                        auto back = panel.x;
                        rich::unlimit_fill_proc<Copy>(data, size, dest, tail, back, cell::shaders::full);
                    }
                    coord.y = std::min(coord.y + y_end + 1, panel.y - 1);
                    // Note: coord can be unsync due to scroll regions.
                }
                assert(test_coord());
            }
            // scroll_buf: .
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
                if (coord.y < y_top)
                {
                    fill(upbox.begin(), coord.x + coord.y * panel.x);
                }
                else if (coord.y <= y_end)
                {
                    auto& curln = batch.current();
                    auto  start = batch.caret;
                    advance_tail_fill(curln, start, count);
                    auto newlen = batch.caret + count;
                    if (newlen > curln.length())
                    {
                        curln.crop(newlen, brush.spare.spc());
                        auto& mapln = index[coord.y - y_top];
                        mapln.width = newlen % panel.x;
                        batch.recalc(curln);
                    }
                    fill(curln.begin(), start);
                }
                else
                {
                    fill(dnbox.begin(), coord.x + (coord.y - (y_end + 1)) * panel.x);
                }
            }
            // scroll_buf: .
            auto& _fragment_from_current_coord(si32 left_cells)
            {
                if (coord.y < y_top)
                {
                    upbox.copy_piece(tail_frag, coord.x + coord.y * panel.x, left_cells);
                }
                else if (coord.y <= y_end)
                {
                    auto& curln = batch.current();
                    auto  start = batch.caret;
                    curln.copy_piece(tail_frag, start, left_cells);
                }
                else
                {
                    dnbox.copy_piece(tail_frag, coord.x + (coord.y - (y_end + 1)) * panel.x, left_cells);
                }
                return tail_frag;
            }
            // scroll_buf: Insert text using the specified cell shader.
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
                    sync_coord();
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
            // scroll_buf: Proceed new text (parser callback).
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
                else sync_coord();
            }
            // scroll_buf: Clear scrollback.
            void clear_all() override
            {
                batch.clear();
                reset_scroll_region();
                bufferbase::clear_all();
                resize_history(owner.defcfg.def_length, owner.defcfg.def_growdt, owner.defcfg.def_growmx);
            }
            // scroll_buf: Set scrollback limits.
            void resize_history(si32 new_size, si32 grow_by = 0, si32 grow_mx = 0)
            {
                static constexpr auto BOTTOM_ANCHORED = true;
                new_size = std::max(new_size, panel.y);
                termconfig::recalc_buffer_metrics(new_size, grow_by, grow_mx);
                batch.resize<BOTTOM_ANCHORED>(new_size, grow_by, grow_mx);
                index_rebuild();
            }
            // scroll_buf: Render to the canvas.
            void output(face& dest) override
            {
                dest.vsize(batch.vsize + sctop + scend); // Include margins and bottom oversize.
                auto clip = dest.clip();
                if (!clip) return;
                auto full = dest.full();
                auto coor = twod{ 0, batch.slide - batch.ancdy + y_top };
                auto head = batch.iter_by_id(batch.ancid);
                auto tail = batch.end();
                auto find = selection_active() && match.length() && owner.selmod == mime::textonly;
                auto clip2 = clip;
                clip2.coor.y += sctop;
                clip2.size.y = std::max(0, clip2.size.y - sctop - scend);
                auto stop = clip2.coor.y + clip2.size.y;
                dest.clip(clip2);
                auto fill = [&](auto& area, auto chr)
                {
                    if (auto r = clip2.trim(area))
                    {
                        dest.fill(r, [&](auto& c){ c.txt(chr).fgc(tint::greenlt); });
                    }
                };
                auto left_edge = clip.coor.x;
                auto rght_edge = clip.coor.x + clip.size.x;
                auto half_size = full.size.x / 2;
                auto left_rect = rect{{ left_edge, full.coor.y + coor.y }, dot_11 };
                auto rght_rect = left_rect;
                rght_rect.coor.x += clip.size.x - 1;
                while (head != tail && rght_rect.coor.y < stop)
                {
                    auto& curln = *head;
                    auto height = curln.height(panel.x);
                    auto length = curln.length();
                    auto adjust = curln.style.jet();
                    output_line(dest, curln, coor, cell::shaders::flat);
                    //dest.output_proxy(curln, coor, [&](auto const& coord, auto const& subblock, auto isr_to_l)
                    //{
                    //    dest.text(coord, subblock, isr_to_l, cell::shaders::fusefull);
                    //});
                    if (find)
                    {
                        match.style.wrp(curln.style.wrp());
                        auto offset = si32{ 0 };
                        auto work = [&](auto shader)
                        {
                            while (curln.find(match, offset))
                            {
                                auto c = coor + offset_to_screen(curln, offset);
                                dest.output(match, c, shader);
                                offset += match.length();
                            }
                        };
                        _shade(owner.defcfg.def_find_f, owner.defcfg.def_find_c, work);
                    }

                    if (length > 0) // Highlight the lines that are not shown in full.
                    {
                        rght_rect.size.y = left_rect.size.y = height;
                        if (height == 1)
                        {
                            auto lt_dot = full.coor.x;
                                 if (adjust == bias::center) lt_dot += half_size - length / 2;
                            else if (adjust == bias::right)  lt_dot += full.size.x - length;

                            if (left_edge > lt_dot         ) fill(left_rect, '<');
                            if (rght_edge < lt_dot + length) fill(rght_rect, '>');
                        }
                        else
                        {
                            auto lt_dot = full.coor.x;
                            auto rt_dot = lt_dot + clip.size.x;
                            auto remain = (length - 1) % clip.size.x + 1;
                            if (left_edge > lt_dot)
                            {
                                if ((adjust == bias::right  && left_edge <= rt_dot - remain)
                                 || (adjust == bias::center && left_edge <= lt_dot + half_size - remain / 2))
                                {
                                    --left_rect.size.y;
                                }
                                fill(left_rect, '<');
                            }
                            if (rght_edge < rt_dot)
                            {
                                if ((adjust == bias::left   && rght_edge >= lt_dot + remain)
                                 || (adjust == bias::center && rght_edge >= lt_dot + remain + half_size - remain / 2))
                                {
                                    --rght_rect.size.y;
                                }
                                fill(rght_rect, '>');
                            }
                        }
                    }
                              coor.y += height;
                    rght_rect.coor.y += height;
                    left_rect.coor.y = rght_rect.coor.y;
                    ++head;
                }
                dest.clip(clip);

                if (panel.y != arena) // The scrolling region is set.
                {
                    auto destcoor = clip.coor - dest.coor();
                    auto top_coor = twod{ 0, y_top - sctop } + destcoor;
                    auto end_coor = twod{ 0, y_end + 1     } + destcoor;
                    upbox.move(top_coor);
                    dnbox.move(end_coor);
                    dest.plot(upbox, cell::shaders::xlucent(owner.defcfg.def_lucent));
                    dest.plot(dnbox, cell::shaders::xlucent(owner.defcfg.def_lucent));
                    if (find)
                    {
                        auto draw = [&](auto const& block)
                        {
                            if (auto area = block.area())
                            {
                                auto block_clip = clip;
                                block_clip.size.x = area.size.x; // Follow wrapping for matches.
                                dest.full(block_clip);
                                area.coor -= destcoor;
                                auto offset = si32{};
                                auto marker = [&](auto shader)
                                {
                                    while (block.find(match, offset))
                                    {
                                        auto c = block.toxy(offset) + area.coor;
                                        dest.output(match, c, shader);
                                        offset += match.length();
                                    }
                                };
                                _shade(owner.defcfg.def_find_f, owner.defcfg.def_find_c, marker);
                            }
                        };
                        draw(upbox);
                        draw(dnbox);
                        dest.full(full);
                    }
                }

                selection_render(dest);
            }
            // scroll_buf: Remove all lines below (including futures) except the current. "ED2 Erase viewport" keeps empty lines.
            void del_below() override
            {
                assert(test_futures());

                auto blank = brush.dry().link(parser::brush.link());
                auto clear = [&](twod coor)
                {
                    auto& from = index[coor.y];
                    auto topid = from.index;
                    auto start = from.start;
                    auto i = batch.index_by_id(topid);
                    auto n = batch.size - 1 - i;      // The number of paragraphs below that should pop.
                    auto m = index.size - 1 - coor.y; // The number of visible rows to zero out.
                    auto p = arena      - 1 - coor.y; // The number of new empty rows to add.

                    auto fresh = coor.x == 0 && start != 0;
                    if (fresh) // Remove the index of the current line if the entire visible line is going to be removed.
                    {
                        ++m;
                        ++p;
                    }

                    assert(n >= 0 && n <  batch.size);
                    assert(m >= 0 && m <= index.size);
                    assert(p >= 0 && p <= arena);

                    while (n--) batch.pop_back();
                    while (m--) index.pop_back();

                    auto fills = blank.bgc() != brush.fresh.bgc();
                    if (fills) add_lines(p, blank); // Fill with non-default background.
                    else       add_lines(p);

                    i = batch.index_by_id(topid); // The index may be outdated due to the ring.
                    auto& curln = batch[i];
                    if (fresh)
                    {
                        curln.trimto(start, brush.spc());
                    }
                    else
                    {
                        auto& mapln = index[coor.y];
                        if (fills)
                        {
                            mapln.width = panel.x;
                            auto x = std::min(coor.x, panel.x); // Trim unwrapped lines by viewport.
                            curln.crop(start + x, blank);
                        }
                        else
                        {
                            mapln.width = coor.x;
                            curln.trimto(start + coor.x, brush.spc());
                        }
                        assert(mapln.start == 0 || curln.wrapped());
                    }
                    batch.recalc(curln);
                    index_rebuild();

                    sync_coord();

                    assert(batch.vsize - batch.basis - index.size == 0); // stash
                    assert(test_futures());
                    dnbox.wipe(blank);
                };

                auto coor = coord;
                if (coor.y < y_top)
                {
                    assert(coor.x + coor.y * upbox.size().x < sctop * upbox.size().x);
                    upbox.del_below(coor, blank);
                    clear(dot_00);
                }
                else if (coor.y <= y_end)
                {
                    coor.x = std::max(0, coor.x);
                    coor.y -= y_top;
                    clear(coor);
                }
                else
                {
                    coor.y -= y_end + 1;
                    assert(coor.x + coor.y * dnbox.size().x < scend * dnbox.size().x);
                    dnbox.del_below(coor, blank);
                }
            }
            // scroll_buf: Clear all lines from the viewport top line to the current line.
            void del_above() override
            {
                auto blank = brush.dry(); // Like in altbuf.
                auto clear = [&](twod from)
                {
                    auto head = index.begin();
                    auto tail = head + from.y;
                    while (head != tail)
                    {
                        auto& mapln = *head++;
                        auto& curln = batch.item_by_id(mapln.index);
                        mapln.width = panel.x;
                        curln.splice<true>(mapln.start, panel.x, blank);
                        batch.recalc(curln);
                    }
                    if (from.x > 0)
                    {
                        auto& mapln = *head;
                        auto& curln = batch.item_by_id(mapln.index);
                        mapln.width = std::max(mapln.width, from.x);
                        curln.splice<true>(mapln.start, from.x, blank);
                        batch.recalc(curln);
                    }
                    upbox.wipe(blank);
                };

                auto coor = coord;
                if (coor.x < panel.x) coor.x += 1; // Clear the cell at the current position. See ED1 description.
                if (coor.y < y_top)
                {
                    assert(coor.x + coor.y * upbox.size().x < sctop * upbox.size().x);
                    upbox.del_above(coor, blank);
                }
                else if (coor.y <= y_end)
                {
                    coor.x = std::clamp(coor.x, 0, panel.x);
                    coor.y -= y_top;
                    clear(coor);
                }
                else
                {
                    coor.y -= y_end + 1;
                    assert(coor.x + coor.y * dnbox.size().x < scend * dnbox.size().x);
                    dnbox.del_above(coor, blank);
                    clear(twod{ panel.x , arena - 1 });
                }
            }
            // scroll_buf: Dissect auto-wrapped lines above the specified row in scroll region (incl last line+1).
            void dissect(si32 y_pos)
            {
                assert(y_pos >= 0 && y_pos <= arena);

                auto split = [&](id_t curid, si32 start)
                {
                    auto after = batch.index_by_id(curid);
                    auto tmpln = std::move(batch[after]);
                    auto curit = batch.ring::insert(after + 1, tmpln.index, tmpln.style, parser::brush);
                    auto endit = batch.end();

                    auto& newln = *curit;
                    newln.splice(0, tmpln.substr(start), cell::shaders::full, brush.spc());
                    newln.reset_fill();
                    batch.undock_base_back(tmpln);
                    batch.invite(newln);

                    if (curit != batch.begin())
                    {
                        auto& curln = *(curit - 1);
                        curln = std::move(tmpln);
                        curln.trimto(start, brush.spc());
                        curln.reset_fill();
                        batch.invite(curln);
                    }

                    do  ++(curit++->index);
                    while (curit != endit);

                    assert(test_index());
                };

                if (y_pos < index.size)
                {
                    auto& mapln = index[y_pos];
                    auto  start = mapln.start;
                    auto  curid = mapln.index;
                    if (start == 0) return;

                    split(curid, start);

                    mapln.index++;
                    mapln.start = 0;
                    index_rebuild_from(y_pos);

                    sync_coord();
                }
                else if (y_pos == arena
                      && y_pos <  batch.vsize - batch.basis)
                {
                    auto stash = batch.vsize - batch.basis - index.size;
                    assert(stash >= 0);
                    if (stash == 0) return;

                    auto& mapln = index.back();
                    auto  curid = mapln.index;
                    auto& curln = batch.item_by_id(curid);
                    auto  start = mapln.start + mapln.width;
                    if (start < curln.length())
                    {
                        split(curid, start);
                    }
                }

                assert(test_futures());
                // Note: coord is unsynced -- see set_scroll_region()
            }
            // scroll_buf: Scroll the specified region by n lines (basis - n). The scrollback can only be used with the whole scrolling region.
            void scroll_region(si32 top, si32 end, si32 n, bool use_scrollback) override
            {
                assert(top >= y_top && end <= y_end);

                if (n == 0) return;

                auto stash = arena - (batch.vsize - batch.basis);
                if (stash > 0) add_lines(stash); // Fill-up the scrolling region in order to simplify implementation (dissect() requirement).
                assert(arena == index.size);

                auto count = std::abs(n);
                if (n < 0) // Scroll text up.
                {
                    if (top == y_top && end == y_end && use_scrollback)
                    {
                        count -= feed_futures(count);
                        if (count > 0)
                        {
                            add_lines(count);
                            // Cut, as the ring is used.
                            batch.basis = std::min(batch.basis + count, batch.vsize - arena);
                        }
                    }
                    else
                    {
                        top -= y_top;
                        end -= y_top - 1;

                        auto max = end - top;
                        if (count > max) count = max;

                        auto mdl = top + count;
                        dissect(top);
                        dissect(mdl);
                        dissect(end);

                        // Delete block.
                        auto topid = index[top    ].index;
                        auto mdlid = index[mdl - 1].index + 1;
                        auto endid = index[end - 1].index + 1;
                        auto start = batch.index_by_id(topid);
                        auto range = (si32)(mdlid - topid);
                        auto floor = batch.index_by_id(endid) - range;
                        batch.remove(start, range);

                        // Insert block.
                        while (count-- > 0) batch.insert(floor, id_t{}, parser::style, parser::brush);

                        batch.reindex(start); //todo revise ? The index may be outdated due to the ring.
                        index_rebuild();
                    }
                }
                else // Scroll text down.
                {
                    if (top == y_top && end == y_end && use_scrollback && batch.basis >= n) // Just move the viewport up.
                    {
                        batch.basis -= n;
                    }
                    else
                    {
                        top -= y_top;
                        end -= y_top - 1;

                        auto max = end - top;
                        if (count > max) count = max;

                        auto mdl = end - count;
                        dissect(top);
                        dissect(mdl);
                        dissect(end);

                        // Delete block.
                        auto topid = index[top    ].index;
                        auto endid = index[end - 1].index + 1;
                        auto mdlid = mdl > 0 ? index[mdl - 1].index + 1 // mdl == 0 or mdl == top when count == max (full arena).
                                             : topid;
                        auto start = batch.index_by_id(topid);
                        auto range = (si32)(endid - mdlid);
                        auto floor = batch.index_by_id(endid) - range;
                        batch.remove(floor, range);

                        // Insert block.
                        while (count-- > 0) batch.insert(start, id_t{}, parser::style, parser::brush);

                        batch.reindex(start); //todo revise ? The index may be outdated due to the ring.
                    }
                    index_rebuild();
                }

                assert(test_futures());
                assert(test_coord());
            }
            // scroll_buf: Return cell state under cursor.
            cell cell_under_cursor() override
            {
                auto& curln = batch.current();
                auto c = curln.length() && batch.caret <= curln.length() ? curln.at(std::clamp(batch.caret, 0, curln.length() - 1)) : parser::brush;
                return c;
            }
            // scroll_buf: Clear scrollback keeping current line.
            void clear_scrollback() override
            {
                batch.clear_but_current();
                resize_viewport(panel, true);
            }
            //text get_current_line() override
            //{
            //    auto crop = escx{};
            //    auto& stripe = batch.current();
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

            // scroll_buf: Calc grip position by coor.
            auto selection_coor_to_grip(twod coor, grip::type role = grip::base)
            {
                auto link = batch.front().index;
                if (coor.y < 0)
                {
                    return grip{ .link = link,
                                 .coor = coor,
                                 .role = role };
                }
                auto i_cur = batch.index_by_id(batch.ancid);
                assert(i_cur < batch.size);
                auto vtpos = batch.slide - batch.ancdy + y_top;
                auto mxpos = batch.slide + panel.y;
                auto start = batch.begin() + i_cur;
                auto limit = batch.end();
                while (vtpos < mxpos)
                {
                    auto& curln = *start;
                    auto newpos = vtpos + curln.height(panel.x);
                    if ((coor.y >= vtpos && coor.y < newpos) || ++start == limit)
                    {
                        link = curln.index;
                        break;
                    }
                    vtpos = newpos;
                }
                coor.y -= vtpos;
                return grip{ .link = link,
                             .coor = coor,
                             .role = role };
            }
            // scroll_buf: Return scrollbuffer grips.
            auto selection_take_grips()
            {
                if (upmid.role == grip::idle) return std::pair{ dot_mx, dot_mx };
                auto i_cur = batch.index_by_id(batch.ancid);
                auto i_top = batch.index_by_id(upmid.link);
                auto i_end = batch.index_by_id(dnmid.link);
                if (i_top < 0 && i_end < 0)
                {
                    selection_cancel();
                    return std::pair{ dot_mx, dot_mx };
                }
                auto coor1 = upmid.coor;
                auto coor2 = dnmid.coor;
                auto start = batch.begin() + i_cur;
                auto limit = batch.end();
                auto topid = batch.front().index;
                auto endid = batch.back().index;
                auto vtpos = batch.slide - batch.ancdy + y_top;
                auto mxpos = batch.slide + panel.y;
                auto done1 = true;
                auto done2 = true;
                auto check = [&](auto height, auto& curln, auto& undone, auto& grip, auto& coor)
                {
                    if (undone && grip.link == curln.index)
                    {
                        undone = faux;
                        if (grip.coor.y >= height && grip.link != endid) // Try to re anchor it.
                        {
                            auto head = start + 1;
                            auto ypos = grip.coor.y - height;
                            assert(head != limit);
                            while (true)
                            {
                                auto& line = *head;
                                auto line_height = line.height(panel.x);
                                if (ypos < line_height || ++head == limit)
                                {
                                    grip.link = line.index;
                                    grip.coor.y = ypos;
                                    break;
                                }
                                ypos -= line_height;
                            }
                        }
                        coor.y = vtpos + grip.coor.y;
                    }
                };
                // Check the buffer ring.
                if      (i_top < 0)           upmid.link = topid;
                else if (i_top >= batch.size) upmid.link = endid;
                if      (i_end < 0)           dnmid.link = topid;
                else if (i_end >= batch.size) dnmid.link = endid;

                coor1.y = i_top < i_cur ? -dot_mx.y : dot_mx.y;
                coor2.y = i_end < i_cur ? -dot_mx.y : dot_mx.y;

                while (start != limit && vtpos < mxpos && (done1 || done2))
                {
                    auto& curln = *start;
                    auto height = curln.height(panel.x);
                    check(height, curln, done1, upmid, coor1);
                    check(height, curln, done2, dnmid, coor2);
                    vtpos += height;
                    ++start;
                }

                //auto square = rect{ -owner.base::oversz.corner(), owner.base::size() + owner.base::oversz };
                auto square = rect{ .size = owner.base::size() } + owner.base::oversz;
                auto minlim = square.coor;
                auto maxlim = minlim + std::max(dot_00, square.size - dot_11);
                coor1 = std::clamp(coor1, minlim, maxlim);
                coor2 = std::clamp(coor2, minlim, maxlim);
                return std::pair{ coor1, coor2 };
            }
            auto selection_take_span()
            {
                auto span = selspan
                {
                    .place = place,
                    .upmid = upmid,
                    .dnmid = dnmid,
                    .uptop = uptop,
                    .dntop = dntop,
                    .upend = upend,
                    .dnend = dnend,
                };
                if (span.uptop.role == grip::base
                 && span.dntop.role == grip::base)
                {
                    auto basis = twod{ -owner.origin.x, batch.slide };
                    span.head = span.uptop.coor + basis;
                    span.tail = span.dntop.coor + basis;
                }
                else if (span.upmid.role == grip::base
                      && span.dnmid.role == grip::base)
                {
                    std::tie(span.head, span.tail) = selection_take_grips();
                }
                else
                {
                    auto basis = twod{ -owner.origin.x, batch.slide + y_top + arena };
                    span.head = span.upend.coor + basis;
                    span.tail = span.dnend.coor + basis;
                }
                return span;
            }
            void selection_set_span(selspan const& span)
            {
                place = span.place;
                upmid = span.upmid;
                dnmid = span.dnmid;
                uptop = span.uptop;
                dntop = span.dntop;
                upend = span.upend;
                dnend = span.dnend;
            }
            void selection_drag_store()
            {
                dragbase = selection_take_span();
                dragset = true;
            }
            void selection_drag_merge(selspan const& edge)
            {
                auto earlier = [](twod const& a, twod const& b)
                {
                    return a.y < b.y
                        || (a.y == b.y && a.x < b.x);
                };
                selection_set_span(dragbase);
                if (earlier(edge.head, dragbase.head))
                {
                    selection_follow(edge.head, faux);
                    selection_extend(edge.head, faux);
                }
                else
                {
                    selection_follow(edge.tail, faux);
                    selection_extend(edge.tail, faux);
                }
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            // scroll_buf: Start text selection.
            void selection_create(twod coor, bool mode) override
            {
                auto scrolling_margin = batch.slide + y_top;
                if (coor.y < scrolling_margin) // Inside the top margin.
                {
                    place = part::top;
                    coor -= {-owner.origin.x, batch.slide };
                    upmid.role = dnmid.role = grip::idle;
                    upend.role = dnend.role = grip::idle;
                    uptop.role = grip::base;
                    uptop.coor = coor;
                    dntop = uptop;
                }
                else if (coor.y < scrolling_margin + arena) // Inside the scrolling region.
                {
                    place = part::mid;
                    uptop.role = dntop.role = grip::idle;
                    upend.role = dnend.role = grip::idle;
                    upmid = selection_coor_to_grip(coor, grip::base);
                    dnmid = upmid;
                }
                else // Inside the bottom margin.
                {
                    place = part::end;
                    coor -= {-owner.origin.x, scrolling_margin + arena };
                    upmid.role = dnmid.role = grip::idle;
                    uptop.role = dntop.role = grip::idle;
                    upend.role = grip::base;
                    upend.coor = coor;
                    dnend = upend;
                }
                selection_selbox(mode);
                selection_update();
            }
            // scroll_buf: Extend text selection.
            bool selection_extend(twod coor, bool mode) override
            {
                auto x2 = coor.x;
                auto ok = selection_active();
                if (ok)
                {
                    selection_selbox(mode);
                    auto scrolling_margin = batch.slide + y_top;
                    auto edge1 = twod{ dot_mx.x, y_top - 1 };
                    auto edge2 = twod{-dot_mx.x, scrolling_margin };
                    auto edge3 = twod{ dot_mx.x, scrolling_margin + arena - 1 };
                    auto edge4 = twod{-dot_mx.x, 0 };
                    auto set_grip_coor_and_role = [](twod c, grip::type r)
                    {
                        return grip{ .coor = c, .role = r };
                    };
                    if (coor.y < scrolling_margin) // Hit the top margin.
                    {
                        coor -= {-owner.origin.x, batch.slide };
                        if (place == part::mid)
                        {
                            if (uptop.role == grip::base)
                            {
                                upmid.role = dnmid.role = grip::idle;
                                upend.role = dnend.role = grip::idle;
                            }
                            else if (upmid.role == grip::base || upend.role != grip::idle)
                            {
                                uptop = set_grip_coor_and_role(edge1, grip::join);
                                dnmid = selection_coor_to_grip(edge2, grip::join);
                            }
                        }
                        else if (place == part::end)
                        {
                            if (uptop.role == grip::base)
                            {
                                upmid.role = dnmid.role = grip::idle;
                                upend.role = dnend.role = grip::idle;
                            }
                            else if (upmid.role == grip::base)
                            {
                                uptop = set_grip_coor_and_role(edge1, grip::join);
                                dnmid = selection_coor_to_grip(edge2, grip::join);
                                upend.role = dnend.role             = grip::idle;
                            }
                            else if (upend.role == grip::base)
                            {
                                uptop = set_grip_coor_and_role(edge1, grip::join);
                                dnmid = selection_coor_to_grip(edge2, grip::join);
                                upmid = selection_coor_to_grip(edge3, grip::join);
                                dnend = set_grip_coor_and_role(edge4, grip::join);
                            }
                        }
                        else if (place == part::top && upmid.role != grip::idle)
                        {
                            dnmid = selection_coor_to_grip(edge2, grip::join);
                        }
                        dntop.coor = coor; dntop.role = grip::base;
                        place = part::top;
                    }
                    else if (coor.y < scrolling_margin + arena) // Hit the scrolling region.
                    {
                        if (place == part::mid)
                        {
                            dnmid = selection_coor_to_grip(coor, grip::base);
                        }
                        else if (place == part::top)
                        {
                            if (uptop.role == grip::base)
                            {
                                dntop = set_grip_coor_and_role(edge1, grip::join);
                                upmid = selection_coor_to_grip(edge2, grip::join);
                                dnmid = selection_coor_to_grip(coor , grip::base);
                                upend.role = dnend.role             = grip::idle;
                            }
                            else if (uptop.role == grip::join)
                            {
                                uptop.role = dntop.role            = grip::idle;
                                dnmid = selection_coor_to_grip(coor, grip::base);
                            }
                        }
                        else if (place == part::end)
                        {
                            if (upend.role == grip::base)
                            {
                                uptop.role = dntop.role             = grip::idle;
                                dnmid = selection_coor_to_grip(coor , grip::base);
                                upmid = selection_coor_to_grip(edge3, grip::join);
                                dnend = set_grip_coor_and_role(edge4, grip::join);
                            }
                            else if (upend.role == grip::join)
                            {
                                upend.role = dnend.role            = grip::idle;
                                dnmid = selection_coor_to_grip(coor, grip::base);
                            }
                        }
                        place = part::mid;
                    }
                    else // Hit the bottom margin.
                    {
                        coor -= {-owner.origin.x, scrolling_margin + arena };
                        if (place == part::mid)
                        {
                            if (upend.role == grip::base)
                            {
                                upmid.role = dnmid.role = grip::idle;
                                uptop.role = dntop.role = grip::idle;
                            }
                            else if (dnmid.role == grip::base || uptop.role != grip::idle)
                            {
                                dnmid = selection_coor_to_grip(edge3, grip::join);
                                upend = set_grip_coor_and_role(edge4, grip::join);
                            }
                        }
                        else if (place == part::top)
                        {
                            if (upend.role == grip::base)
                            {
                                upmid.role = dnmid.role = grip::idle;
                                uptop.role = dntop.role = grip::idle;
                            }
                            else if (upmid.role == grip::base)
                            {
                                uptop.role = dntop.role             = grip::idle;
                                dnmid = selection_coor_to_grip(edge3, grip::join);
                                dnend = set_grip_coor_and_role(edge4, grip::join);
                            }
                            else if (uptop.role == grip::base)
                            {
                                dntop = set_grip_coor_and_role(edge1, grip::join);
                                upmid = selection_coor_to_grip(edge2, grip::join);
                                dnmid = selection_coor_to_grip(edge3, grip::join);
                                upend = set_grip_coor_and_role(edge4, grip::join);
                            }
                        }
                        else if (place == part::end && upmid.role != grip::idle)
                        {
                            dnmid = selection_coor_to_grip(edge3, grip::join);
                        }
                        dnend.coor = coor; dnend.role = grip::base;
                        place = part::end;
                    }

                    if (panel.y != arena)
                    {
                        if (selection_selbox())
                        {
                            auto x1 = upmid.role == grip::base ? upmid.coor.x :
                                      uptop.role == grip::base ? uptop.coor.x - owner.origin.x:
                                                                 upend.coor.x - owner.origin.x;
                            if (upmid.role != grip::base) upmid.coor.x = x1;
                            if (dnmid.role != grip::base) dnmid.coor.x = x2;
                            if (uptop.role != grip::base) uptop.coor.x = x1 + owner.origin.x;
                            if (dntop.role != grip::base) dntop.coor.x = x2 + owner.origin.x;
                            if (upend.role != grip::base) upend.coor.x = x1 + owner.origin.x;
                            if (dnend.role != grip::base) dnend.coor.x = x2 + owner.origin.x;
                        }
                        else
                        {
                            if (dnmid.role == grip::join)
                            {
                                if (upend.role == grip::join)
                                {
                                    dnmid.coor.x = dot_mx.x;
                                    upend.coor.x =-dot_mx.x;
                                }
                                if (uptop.role == grip::join)
                                {
                                    dnmid.coor.x =-dot_mx.x;
                                    uptop.coor.x = dot_mx.x;
                                }
                            }
                            if (upmid.role == grip::join)
                            {
                                if (dnend.role == grip::join)
                                {
                                    upmid.coor.x = dot_mx.x;
                                    dnend.coor.x =-dot_mx.x;
                                }
                                if (dntop.role == grip::join)
                                {
                                    upmid.coor.x =-dot_mx.x;
                                    dntop.coor.x = dot_mx.x;
                                }
                            }
                        }
                    }

                    selection_update();
                }
                return ok;
            }
            // scroll_buf: Set selection orientation.
            void selection_follow(twod coor, bool lock) override
            {
                selection_locked(lock);
                if (selection_active())
                {
                    auto swap = faux;
                    auto scrolling_margin = batch.slide + y_top;
                    if (uptop.role == grip::base
                     && dntop.role == grip::base)
                    {
                        auto p = coor - twod{-owner.origin.x, batch.slide };
                        swap = dntop.coor.y == uptop.coor.y ? std::abs(dntop.coor.x - p.x) > std::abs(uptop.coor.x - p.x)
                                                            : std::abs(dntop.coor.y - p.y) > std::abs(uptop.coor.y - p.y);
                    }
                    else if (upend.role == grip::base
                          && dnend.role == grip::base)
                    {
                        auto p = coor - twod{-owner.origin.x, scrolling_margin + arena };
                        swap = dnend.coor.y == upend.coor.y ? std::abs(dnend.coor.x - p.x) > std::abs(upend.coor.x - p.x)
                                                            : std::abs(dnend.coor.y - p.y) > std::abs(upend.coor.y - p.y);
                    }
                    else if (coor.y < scrolling_margin || coor.y >= scrolling_margin + arena)
                    {
                        if (((dntop.role == grip::join || upend.role == grip::join) && coor.y < scrolling_margin)
                         || ((uptop.role == grip::join || dnend.role == grip::join) && coor.y >= scrolling_margin + arena))
                        {
                            swap = true;
                        }
                        else
                        {
                            auto idtop = batch.index_by_id(upmid.link);
                            auto idend = batch.index_by_id(dnmid.link);
                            auto order = idtop != idend ? idend > idtop
                                                        : dnmid.coor.y != upmid.coor.y ? dnmid.coor.y > upmid.coor.y
                                                                                       : dnmid.coor.x > upmid.coor.x;
                            swap = (coor.y < scrolling_margin) == order;
                        }
                    }
                    else
                    {
                        auto check = selection_coor_to_grip(coor);
                        auto idtop = batch.index_by_id(upmid.link);
                        auto idend = batch.index_by_id(dnmid.link);
                        auto idcur = batch.index_by_id(check.link);

                        if (idtop != idend)
                        {
                            auto cy = (idend + idtop) / 2;
                            swap = (idcur > cy) == (idtop > idend);
                        }
                        else // idend == idtop
                        {
                            if (idend == idcur)
                            {
                                if (dnmid.coor.y != upmid.coor.y)
                                {
                                    auto cy = (dnmid.coor.y + upmid.coor.y) / 2;
                                    swap = (check.coor.y > cy) == (upmid.coor.y > dnmid.coor.y);
                                }
                                else
                                {
                                    swap = (upmid.coor.y == check.coor.y ? std::abs(dnmid.coor.x - check.coor.x) > std::abs(upmid.coor.x - check.coor.x)
                                                                         : std::abs(dnmid.coor.y - check.coor.y) > std::abs(upmid.coor.y - check.coor.y));
                                }
                            }
                            else swap = (idcur > idend) == (upmid.coor.y > dnmid.coor.y);
                        }
                    }

                    if (swap)
                    {
                        std::swap(uptop, dntop);
                        std::swap(upmid, dnmid);
                        std::swap(upend, dnend);
                        place = dntop.role == grip::base ? part::top
                              : dnmid.role == grip::base ? part::mid
                                                         : part::end;
                    }
                    if (selection_selbox())
                    {
                        auto x = coor.x + owner.origin.x;
                        auto c = (upmid.coor.x + dnmid.coor.x) / 2;
                        if ((x > c) == (upmid.coor.x > dnmid.coor.x))
                        {
                            std::swap(uptop.coor.x, dntop.coor.x);
                            std::swap(upmid.coor.x, dnmid.coor.x);
                            std::swap(upend.coor.x, dnend.coor.x);
                        }
                    }
                }
            }
            // scroll_buf: Select one word.
            void selection_byword(twod coor) override
            {
                auto scrolling_margin = batch.slide + y_top;
                if (coor.y < scrolling_margin) // Inside the top margin.
                {
                    place = part::top;
                    coor -= {-owner.origin.x, batch.slide };
                    upmid.role = dnmid.role = grip::idle;
                    upend.role = dnend.role = grip::idle;
                    uptop.role = grip::base;
                    uptop.coor = coor;
                    dntop = uptop;
                    uptop.coor.x = upbox.word<feed::rev>(coor);
                    dntop.coor.x = upbox.word<feed::fwd>(coor);
                }
                else if (coor.y < scrolling_margin + arena) // Inside the scrolling region.
                {
                    place = part::mid;
                    uptop.role = dntop.role = grip::idle;
                    upend.role = dnend.role = grip::idle;
                    upmid = selection_coor_to_grip(coor, grip::base);
                    dnmid = upmid;
                    auto& line = batch.item_by_id(upmid.link);
                    auto start = screen_to_offset(line, upmid.coor);
                    auto offup = line.word<feed::rev>({ start, 0 });
                    auto offdn = line.word<feed::fwd>({ start, 0 });
                    upmid.coor = offset_to_screen(line, offup);
                    dnmid.coor = offset_to_screen(line, offdn);
                }
                else // Inside the bottom margin.
                {
                    place = part::end;
                    coor -= {-owner.origin.x, scrolling_margin + arena };
                    upmid.role = dnmid.role = grip::idle;
                    uptop.role = dntop.role = grip::idle;
                    upend.role = grip::base;
                    upend.coor = coor;
                    dnend = upend;
                    upend.coor.x = dnbox.word<feed::rev>(coor);
                    dnend.coor.x = dnbox.word<feed::fwd>(coor);
                }
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            void selection_byword(twod from, twod to) override
            {
                auto earlier = [](twod const& a, twod const& b)
                {
                    return a.y < b.y
                        || (a.y == b.y && a.x < b.x);
                };
                selection_byword(from);
                auto base = selection_take_span();
                selection_byword(to);
                auto edge = selection_take_span();
                selection_set_span(base);
                if (earlier(edge.head, base.head))
                {
                    selection_follow(edge.head, faux);
                    selection_extend(edge.head, faux);
                }
                else
                {
                    selection_follow(edge.tail, faux);
                    selection_extend(edge.tail, faux);
                }
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            void selection_drag_word_start(twod coor) override
            {
                selection_byword(coor);
                selection_drag_store();
            }
            void selection_drag_word_pull(twod coor) override
            {
                if (!dragset)
                {
                    selection_drag_word_start(coor);
                    return;
                }
                selection_byword(coor);
                auto edge = selection_take_span();
                selection_drag_merge(edge);
            }
            // scroll_buf: Select line.
            void selection_byline(twod coor) override
            {
                auto scrolling_margin = batch.slide + y_top;
                if (coor.y < scrolling_margin) // Inside the top margin.
                {
                    place = part::top;
                    coor -= {-owner.origin.x, batch.slide };
                    upmid.role = dnmid.role = grip::idle;
                    upend.role = dnend.role = grip::idle;
                    uptop.role = grip::base;
                    uptop.coor = coor;
                    dntop = uptop;
                    uptop.coor.x = 0;
                    dntop.coor.x = panel.x - 1;
                }
                else if (coor.y < scrolling_margin + arena) // Inside the scrolling region.
                {
                    place = part::mid;
                    uptop.role = dntop.role = grip::idle;
                    upend.role = dnend.role = grip::idle;
                    upmid = selection_coor_to_grip(coor, grip::base);
                    dnmid = upmid;
                    auto& curln = batch.item_by_id(upmid.link);
                    auto limit = std::max(0, curln.length() - 1);
                    upmid.coor = offset_to_screen(curln, 0);
                    dnmid.coor = offset_to_screen(curln, limit);
                }
                else // Inside the bottom margin.
                {
                    place = part::end;
                    coor -= {-owner.origin.x, scrolling_margin + arena };
                    upmid.role = dnmid.role = grip::idle;
                    uptop.role = dntop.role = grip::idle;
                    upend.role = grip::base;
                    upend.coor = coor;
                    dnend = upend;
                    upend.coor.x = 0;
                    dnend.coor.x = panel.x - 1;
                }
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            void selection_byline(twod from, twod to) override
            {
                auto earlier = [](twod const& a, twod const& b)
                {
                    return a.y < b.y
                        || (a.y == b.y && a.x < b.x);
                };
                selection_byline(from);
                auto base = selection_take_span();
                selection_byline(to);
                auto edge = selection_take_span();
                selection_set_span(base);
                if (earlier(edge.head, base.head))
                {
                    selection_follow(edge.head, faux);
                    selection_extend(edge.head, faux);
                }
                else
                {
                    selection_follow(edge.tail, faux);
                    selection_extend(edge.tail, faux);
                }
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            void selection_drag_line_start(twod coor) override
            {
                selection_byline(coor);
                selection_drag_store();
            }
            void selection_drag_line_pull(twod coor) override
            {
                if (!dragset)
                {
                    selection_drag_line_start(coor);
                    return;
                }
                selection_byline(coor);
                auto edge = selection_take_span();
                selection_drag_merge(edge);
            }
            void selection_drag_clear() override
            {
                dragset = faux;
            }
            // scroll_buf: Select all (ignore non-scrolling regions).
            void selection_selall() override
            {
                place = scend ? part::end : part::mid; // Last active region.
                uptop.role = dntop.role = grip::idle;
                upmid.role = dnmid.role = grip::base;
                upend.role = dnend.role = grip::idle;
                if (y_top != 0)
                {
                    uptop.role = grip::base;
                    dntop.role = upmid.role = grip::join;
                    uptop.coor = { 0, sctop - y_top };
                    dntop.coor = { panel.x - 1, sctop - 1 };
                }
                auto dyend = (panel.y - 1) - y_end;
                if (dyend > 0)
                {
                    dnmid.role = upend.role = grip::join;
                    dnend.role = grip::base;
                    upend.coor = { 0, 0 };
                    dnend.coor = { panel.x - 1, dyend };
                }
                auto& topln = batch.front();
                auto& endln = batch.back();
                auto x = std::max(0, endln.length() - 1);
                upmid = { .link = topln.index, .coor = offset_to_screen(topln, 0), .role = upmid.role };
                dnmid = { .link = endln.index, .coor = offset_to_screen(endln, x), .role = dnmid.role };
                selection_locked(faux);
                selection_selbox(faux);
                selection_update(faux);
            }
            // scroll_buf: Return the indexes and a grips copy.
            auto selection_get_it() const
            {
                auto upcur = upmid;
                auto dncur = dnmid;
                auto i_top = batch.index_by_id(upcur.link);
                auto i_end = batch.index_by_id(dncur.link);
                if (i_top < 0)
                {
                    if (i_end < 0) return std::tuple{-1,-1, upcur, dncur };
                    upcur.coor = dot_00;
                }
                else if (i_end < 0)
                {
                    dncur.coor = dot_00;
                }

                i_top = std::clamp(i_top, 0, batch.size - 1);
                i_end = std::clamp(i_end, 0, batch.size - 1);
                if (i_top >  i_end
                || (i_top == i_end && (upcur.coor.y >  dncur.coor.y
                                   || (upcur.coor.y == dncur.coor.y && (upcur.coor.x > dncur.coor.x)))))
                {
                    std::swap(i_top, i_end);
                    std::swap(upcur, dncur);
                }
                return std::tuple{ i_top, i_end, upcur, dncur };
            }
            // scroll_buf: Calc selection height in dislay lines.
            template<class T>
            auto selection_height(T head, T tail, grip const& upcur, grip const& dncur) const
            {
                auto vpos = -upcur.coor.y;
                while (head != tail)
                {
                    vpos += head->height(panel.x);
                    ++head;
                }
                vpos += 1 + dncur.coor.y;
                return vpos;
            }
            // scroll_buf: Calc selection volume in cells.
            template<class T>
            auto selection_volume(T head, T tail, grip const& upcur, grip const& dncur) const
            {
                auto& top = *head;
                auto& end = *tail;
                auto summ = -std::min(top.length(), upcur.coor.y * panel.x + std::max(0, upcur.coor.x));
                auto vpos = -upcur.coor.y;
                while (head != tail)
                {
                    auto& line = *head;
                    vpos += line.height(panel.x);
                    summ += line.length();
                    ++head;
                }
                summ += std::min(end.length(), 1 + dncur.coor.y * panel.x + std::max(0, dncur.coor.x));
                vpos += 1 + dncur.coor.y;
                return std::pair{ vpos, summ };
            }
            // scroll_buf: Calc selection offset in cells.
            auto selection_offset(auto& curln, auto coor, auto close)
            {
                auto align = curln.style.jet();
                auto wraps = curln.style.wrp();
                auto width = curln.length();
                if (wraps == wrap::on)
                {
                    coor.x = std::clamp(coor.x, -close, -close + panel.x);
                    if (align != bias::left && coor.y == width / panel.x)
                    {
                        if (auto remain = width % panel.x)
                        {
                            if (align == bias::right)    coor.x = std::max(0,      coor.x - panel.x     + remain);
                            else      /* bias::center */ coor.x = std::max(-close, coor.x - panel.x / 2 + remain / 2);
                        }
                    }
                }
                else
                {
                    coor.y = 0;
                    if (align != bias::left)
                    {
                        if (align == bias::right)    coor.x -= panel.x     - width;
                        else      /* bias::center */ coor.x -= panel.x / 2 - width / 2;
                    }
                }
                return coor.x + coor.y * panel.x + close;
            }
            // scroll_buf: Make a viewport screen copy.
            void do_viewport_copy(face& dest) override
            {
                auto full = dest.full();
                auto clip = dest.clip().trim(full);
                dest.clip(clip);
                auto vpos = clip.coor.y - y_top;
                if (vpos >= 0 && vpos < arena)
                {
                    auto& mapln = index[vpos];
                    auto  ancid = mapln.index;
                    auto  ancdy = mapln.start / panel.x;
                    auto  limit = clip.coor.y + clip.size.y;
                    auto head = batch.iter_by_id(ancid);
                    auto tail = batch.end();
                    auto coor = twod{ 0, clip.coor.y - ancdy };
                    while (head != tail && coor.y < limit)
                    {
                        auto& curln = *head++;
                        auto height = curln.height(panel.x);
                        output_line(dest, curln, coor);
                        coor.y += height;
                    }
                }
                upbox.move({ 0, y_top - sctop });
                dnbox.move({ 0, y_end + 1     });
                dest.fill(upbox, cell::shaders::full);
                dest.fill(dnbox, cell::shaders::full);
            }
            // scroll_buf: Materialize selection of the scrollbuffer part.
            void selection_pickup(escx& yield, si32 selmod)
            {
                //todo Clang 15 don't get it
                //auto [i_top, i_end, upcur, dncur] = selection_get_it();
                auto tempvr = selection_get_it();
                auto i_top = std::get<0>(tempvr);
                auto i_end = std::get<1>(tempvr);
                auto upcur = std::get<2>(tempvr);
                auto dncur = std::get<3>(tempvr);

                if (i_top == -1) return;

                auto data = batch.begin();
                auto head = data + i_top;
                auto tail = data + i_end;

                if (selection_selbox())
                {
                    auto dest = face{};
                    auto mark = cell{};
                    auto coor = dot_00;
                    auto area = rect{{ std::min(upcur.coor.x,  dncur.coor.x), upcur.coor.y },
                                     { std::abs(upcur.coor.x - dncur.coor.x) + 1, selection_height(head, tail, upcur, dncur) }};
                    auto clip = rect{ dot_00, area.size };
                    auto full = rect{ -area.coor, { panel.x, area.coor.y + area.size.y }};
                    dest.core::size(area.size, brush.spare.dry());
                    dest.core::clip(clip);
                    dest.flow::full(full);
                    do
                    {
                        auto& curln = *head;
                        dest.output(curln, coor);
                        coor.y += curln.height(panel.x);
                    }
                    while (head++ != tail);
                    selmod == mime::disabled ||
                    selmod == mime::textonly ||
                    selmod == mime::safetext ? yield.s11n<faux, faux, true>(dest, mark)
                                             : yield.s11n<true, faux, true>(dest, mark);
                }
                else
                {
                    auto field = rect{ dot_00, dot_01 };
                    auto accum = cell{};
                    auto build = [&](auto print)
                    {
                        if (i_top == i_end)
                        {
                            auto& headln = *head++;
                            field.coor.x = selection_offset(headln, upcur.coor, 0);
                            field.size.x = selection_offset(headln, dncur.coor, 1);
                            field.size.x = field.size.x - field.coor.x;
                            print(headln);
                        }
                        else
                        {
                            auto& headln = *head++;
                            field.coor.x = selection_offset(headln, upcur.coor, 0);
                            field.size.x = dot_mx.x;
                            print(headln);
                            field.coor.x = 0;
                            while (head != tail) print(*head++);
                            auto& lastln = *head++;
                            field.size.x = selection_offset(lastln, dncur.coor, 1);
                            print(lastln);
                        }
                        if (yield.length()) yield.pop_back(); // Pop last eol.
                    };
                    if (selmod == mime::textonly
                     || selmod == mime::safetext
                     || selmod == mime::disabled)
                    {
                        build([&](auto& curln)
                        {
                            auto block = escx{};
                            block.s11n<faux, faux, faux>(curln, field, accum);
                            if (block.size() > 0) yield.add(block);
                            else                  yield.eol();
                        });
                    }
                    else
                    {
                        auto s = deco{};
                        build([&](auto& curln)
                        {
                            if (s != curln.style)
                            {
                                if (auto wrp = curln.style.wrp(); s.wrp() != wrp) yield.wrp(wrp);
                                if (auto jet = curln.style.jet(); s.jet() != jet) yield.jet(jet);
                                s = curln.style;
                            }
                            auto block = escx{};
                            block.s11n<true, faux, faux>(curln, field, accum);
                            if (block.size() > 0) yield.add(block);
                            else                  yield.eol();
                        });
                        yield.nil();
                    }
                }
            }
            // scroll_buf: Materialize selection.
            text selection_pickup(si32 selmod) override
            {
                auto yield = escx{};
                auto selbox = selection_selbox();
                if (!selection_active()) return yield;
                if (selmod != mime::textonly
                 && selmod != mime::safetext) yield.nil();
                auto len = yield.size();
                if (uptop.role != grip::idle)
                {
                    bufferbase::selection_pickup(yield, upbox, uptop.coor, dntop.coor, selmod, selbox);
                }
                if (upmid.role != grip::idle)
                {
                    if (std::exchange(len, yield.size()) != len) yield.eol();
                    scroll_buf::selection_pickup(yield, selmod);
                }
                if (upend.role != grip::idle)
                {
                    if (std::exchange(len, yield.size()) != len) yield.eol();
                    bufferbase::selection_pickup(yield, dnbox, upend.coor, dnend.coor, selmod, selbox);
                }
                if (selbox && std::exchange(len, yield.size()) != len) yield.eol();
                return yield;
            }
            // scroll_buf: Highlight selection.
            void selection_render(face& dest) override
            {
                if (selection_active())
                {
                    auto full = dest.full();
                    auto clip = dest.clip();
                    auto cntx = dest.change_basis(full);
                    auto mode = owner.selmod;
                    if (panel.y != arena) // Draw fixed regions.
                    {
                        auto draw_area = [&](auto grip_1, auto grip_2, auto offset)
                        {
                            if (grip_1.role != grip::idle)
                            {
                                grip_1.coor.y += offset;
                                grip_2.coor.y += offset;
                                grip::sort(grip_1, grip_2);
                                bufferbase::selection_raster(dest, grip_1.coor, grip_2.coor, grip_1.role == grip::base, grip_2.role == grip::base);
                            }
                        };
                        auto dytop = y_top;
                        auto dyend = (panel.y - 1) - y_end;
                        if (dytop > 0) draw_area(uptop, dntop, batch.slide - (sctop - y_top));
                        if (dyend > 0) draw_area(upend, dnend, batch.slide + y_end + 1);
                    }
                    if (upmid.role == grip::idle) return;
                    auto scrolling_region = rect{{ -dot_mx.x / 2, batch.slide + y_top }, { dot_mx.x, arena }};
                    clip.trimby(scrolling_region);
                    //todo Clang 15 don't get it
                    //auto [curtop, curend] = selection_take_grips();
                    auto tempvr = selection_take_grips();
                    auto curtop = tempvr.first;
                    auto curend = tempvr.second;
                    auto grip_1 = rect{ curtop, dot_11 };
                    auto grip_2 = rect{ curend, dot_11 };
                    if (selection_selbox())
                    {
                        auto area = grip_1 | grip_2;
                        auto proc = [&](auto fx)
                        {
                            dest.fill(area.trim(clip), fx);
                        };
                        _shade_selection(mode, proc);
                    }
                    else
                    {
                        if (curtop.y >  curend.y
                        || (curtop.y == curend.y && curtop.x > curend.x))
                        {
                            std::swap(curtop, curend);
                        }
                        dest.vsize(batch.vsize + sctop + scend); // Include margins and bottom oversize.
                        auto coor = twod{ 0, batch.slide - batch.ancdy + y_top };
                        auto stop = batch.slide + arena + y_top;
                        auto head = batch.iter_by_id(batch.ancid);
                        auto tail = batch.end();
                        auto work = [&](auto fill)
                        {
                            auto draw = [&](auto const& coord, auto const& subblock, auto /*isr_to_l*/)
                            {
                                     if (coord.y < curtop.y) return;
                                else if (coord.y > curend.y) coor.y = stop;
                                else
                                {
                                    auto block = rect{ coord, { subblock.length(), 1 }};
                                    if (coord.y == curtop.y)
                                    {
                                        auto width = curtop.y == curend.y ? curend.x - curtop.x + 1
                                                                          : dot_mx.x;
                                        auto bound = rect{ curtop, { width, 1 }}.normalize();
                                        block.trimby(bound);
                                    }
                                    else if (coord.y == curend.y)
                                    {
                                        auto bound = rect{ curend, { -dot_mx.x, 1 }}.normalize();
                                        bound.size.x += 1;
                                        block.trimby(bound);
                                    }
                                    block.trimby(clip);
                                    dest.fill(block, fill);
                                }
                            };
                            while (head != tail && coor.y < stop)
                            {
                                auto& curln = *head;
                                auto length = curln.length();
                                auto height = curln.height(panel.x);
                                if (length)
                                {
                                    dest.output_proxy(curln, coor, draw);
                                }
                                else
                                {
                                    auto align = curln.style.jet();
                                    auto coord = coor;
                                    switch (align)
                                    {
                                        case bias::none:
                                        case bias::left:   break;
                                        case bias::right:  coord.x += panel.x - 1; break;
                                        case bias::center: coord.x += panel.x / 2; break;
                                    }
                                    struct { auto length() const { return 1; }} empty;
                                    draw(coord, empty, faux);
                                }
                                coor.y += height;
                                ++head;
                            }
                        };
                        _shade_selection(mode, work);
                    }
                }
            }
            // scroll_buf: Update selection status.
            void selection_status(term_state& status) const override
            {
                status.coor.x = 1 + std::abs(dnmid.coor.x - upmid.coor.x);
                if (upmid.role != grip::idle)
                {
                    status.coor.y = 1 + std::abs((si32)(dnmid.link - upmid.link));
                    if (status.coor.y < approx_threshold)
                    {
                        auto [i_top, i_end, upcur, dncur] = selection_get_it();
                        auto data = batch.begin();
                        auto head = data + i_top;
                        auto tail = data + i_end;
                        auto [height, volume] = selection_volume(head, tail, upcur, dncur);
                        if (selection_selbox()) status.coor.y = height;
                        else                    status.body   = volume;
                    }
                    else
                    {
                        if (!selection_selbox()) status.body = status.coor.y * panel.x;
                    }
                }
                else
                {
                    status.coor.y = 0;
                    status.body   = 0;
                }
                if (panel.y != arena)
                {
                    auto calc = [&](auto top, auto end)
                    {
                        auto dt = twod{};
                        top.x = std::clamp(top.x, 0, panel.x - 1);
                        end.x = std::clamp(end.x, 0, panel.x - 1);
                        if (top.y == end.y)
                        {
                            dt = { std::abs(end.x - top.x) + 1, 1 };
                        }
                        else
                        {
                            if (top.y > end.y) std::swap(top, end);
                            dt.y = end.y - top.y + 1;
                            dt.x += panel.x * (dt.y - 2);
                            dt.x += panel.x + end.x - top.x + 1;
                        }
                        status.coor.y += dt.y;
                        status.body   += dt.x;
                    };
                    if (uptop.role != grip::idle) calc(uptop.coor, dntop.coor);
                    if (upend.role != grip::idle) calc(upend.coor, dnend.coor);
                }
            }
            // scroll_buf: Loop through selected lines.
            template<class P>
            void selection_foreach(P proc)
            {
                auto [i_top, i_end, upcur, dncur] = selection_get_it();
                if (i_top == -1)
                {
                    selection_cancel();
                }
                else
                {
                    auto data = batch.begin();
                    auto head = data + i_top;
                    auto tail = data + i_end;
                    do proc(*head);
                    while (head++ != tail);
                }
            }
            // scroll_buf: Sel alignment for selected lines.
            void selection_setjet(bias a = bias::none) override
            {
                //todo unify setwrp and setjet
                if (selection_active())
                {
                    if (upmid.role == grip::idle) return;
                    auto i_top = std::clamp(batch.index_by_id(upmid.link), 0, batch.size);
                    auto j = batch[i_top].style.jet();
                    auto align = a != bias::none ? a
                                                 : j == bias::left   ? bias::right
                                                 : j == bias::right  ? bias::center
                                                                     : bias::left;
                    selection_foreach([&](auto& curln)
                    {
                        curln.style.jet(align);
                        batch.recalc(curln);
                    });
                    if (a != bias::none) style.jet(a);
                    resize_viewport(panel, true); // Recalc batch.basis.
                }
                else
                {
                    auto j = style.jet();
                    style.jet(a != bias::none ? a
                                              : j == bias::left   ? bias::right
                                              : j == bias::right  ? bias::center
                                                                  : bias::left);
                }
            }
            // scroll_buf: Sel wrapping mode for selected lines.
            void selection_setwrp(wrap w = wrap::none) override
            {
                if (selection_active())
                {
                    if (upmid.role == grip::idle) return;
                    auto i_top = std::clamp(batch.index_by_id(upmid.link), 0, batch.size);
                    auto wraps = w == wrap::none ? batch[i_top].style.wrp() == wrap::on ? wrap::off : wrap::on
                                                 : w;
                    upmid.coor.y = dnmid.coor.y = 0;
                    selection_foreach([&](auto& curln)
                    {
                        curln.style.wrp(wraps);
                        batch.recalc(curln);
                    });
                    if (w != wrap::none) style.wrp(w);
                    resize_viewport(panel, true); // Recalc batch.basis.
                }
                else
                {
                    style.wrp(w == wrap::none ? style.wrp() == wrap::on ? wrap::off : wrap::on
                                              : w);
                }
            }
            // scroll_buf: Update selection internals.
            void selection_update(bool despace = true) override
            {
                if (!selection_selbox())
                {
                    if (upmid.role == grip::base
                     && dnmid.role == grip::base)
                    {
                        auto up_i = batch.index_by_id(upmid.link);
                        auto dn_i = batch.index_by_id(dnmid.link);
                        if (up_i >= 0 && dn_i >= 0)
                        {
                            auto up_first = up_i < dn_i
                                         || (up_i == dn_i
                                          && (upmid.coor.y < dnmid.coor.y
                                           || (upmid.coor.y == dnmid.coor.y
                                            && upmid.coor.x <= dnmid.coor.x)));
                            if (up_first)
                            {
                                auto& upln = batch.item_by_id(upmid.link);
                                auto& dnln = batch.item_by_id(dnmid.link);
                                normalize_line_edge(upln, upmid.coor, true);
                                normalize_line_edge(dnln, dnmid.coor, faux);
                            }
                            else
                            {
                                auto& dnln = batch.item_by_id(dnmid.link);
                                auto& upln = batch.item_by_id(upmid.link);
                                normalize_line_edge(dnln, dnmid.coor, true);
                                normalize_line_edge(upln, upmid.coor, faux);
                            }
                        }
                    }
                    if (uptop.role == grip::base
                     && dntop.role == grip::base)
                    {
                        auto p1 = uptop.coor;
                        auto p2 = dntop.coor;
                        normalize_canvas_edges(upbox, p1, p2);
                        uptop.coor = p1;
                        dntop.coor = p2;
                    }
                    if (upend.role == grip::base
                     && dnend.role == grip::base)
                    {
                        auto p1 = upend.coor;
                        auto p2 = dnend.coor;
                        normalize_canvas_edges(dnbox, p1, p2);
                        upend.coor = p1;
                        dnend.coor = p2;
                    }
                }
                if (upmid.role == grip::base
                 && dnmid.role == grip::base
                 && upmid.link == dnmid.link
                 && (!selection_selbox() || (upmid.link   == dnmid.link
                                          && upmid.coor.y == dnmid.coor.y)))
                {
                    auto& curln = batch.item_by_id(upmid.link);
                    auto p1 = upmid.coor;
                    auto p2 = dnmid.coor;
                    if (p1.y > p2.y || (p1.y == p2.y && p1.x > p2.x)) std::swap(p1, p2);
                    auto head = selection_offset(curln, p1, 0);
                    auto tail = selection_offset(curln, p2, 1);
                    match = { curln.core::line(head, tail) };
                }
                else if (uptop.role == grip::base
                      && dntop.role == grip::base
                      && (!selection_selbox() || uptop.coor.y == dntop.coor.y))
                {
                    match = { upbox.core::line(uptop.coor, dntop.coor) };
                }
                else if (upend.role == grip::base
                      && dnend.role == grip::base
                      && (!selection_selbox() || upend.coor.y == dnend.coor.y))
                {
                    match = { dnbox.core::line(upend.coor, dnend.coor) };
                }
                else match = {};

                bufferbase::selection_update(despace);
            }
            // scroll_buf: Search data and return distance to it.
            twod selection_gofind(feed direction, view data = {}) override
            {
                if (data.empty()) return dot_00;
                match = line{ data };

                auto ahead = direction == feed::fwd;
                if (ahead)
                {
                    uirev = faux;
                    uptop.coor = dntop.coor = {};
                    uptop.role = dntop.role = grip::base;
                    upmid.role = dnmid.role = grip::idle;
                    upend.role = dnend.role = grip::idle;
                }
                else
                {
                    uifwd = faux;
                    uptop.role = dntop.role = grip::idle;
                    upmid.role = dnmid.role = grip::idle;
                    upend.role = dnend.role = grip::base;
                    upend.coor = dnend.coor = dnbox.size() - dot_01;
                }
                auto delta = selection_gonext(direction);

                if ((ahead && uirev == faux)
                 ||(!ahead && uifwd == faux))
                {
                    selection_cancel();
                    delta = {};
                }
                return delta;
            }
            // scroll_buf: Retrun distance between lines.
            auto selection_outrun(id_t id1, twod coor1, id_t id2, twod coor2)
            {
                auto dir = (si32)(id2 - id1);
                if (dir < 0)
                {
                    std::swap(id1, id2);
                    std::swap(coor1, coor2);
                }
                auto dist = coor2;
                auto head = batch.iter_by_id(id1);
                auto tail = batch.iter_by_id(id2);
                while (head != tail)
                {
                    dist.y += head->height(panel.x);
                    ++head;
                }
                dist -= coor1;
                return dir < 0 ? -dist
                               :  dist;
            }
            // scroll_buf: Retrun viewport center.
            auto selection_viewport_center()
            {
                return twod{ panel.x / 2 - owner.origin.x, arena / 2 + batch.ancdy };
            }
            // scroll_buf: Retrun distance to the center of viewport.
            twod selection_center(id_t line_id, twod coor)
            {
                auto base = selection_viewport_center();
                auto dist = selection_outrun(line_id, coor, batch.ancid, base);
                return dist;
            }
            // scroll_buf: Search prev/next selection match and return distance to it.
            twod selection_gonext(feed direction) override
            {
                if (match.empty()) return dot_00;

                auto delta = dot_00;
                auto ahead = direction == feed::fwd;
                auto probe = [&](auto startid, auto coord)
                {
                    auto& curln = batch.item_by_id(startid);
                    auto from = selection_offset(curln, coord, 0);
                    auto mlen = match.length();
                    auto step = ahead ? mlen : 0;
                    auto back = ahead ? 2 : mlen;
                    auto resx = [&](auto& curln)
                    {
                        if (curln.find(match, from, direction))
                        {
                            upmid.link = curln.index;
                            dnmid.link = curln.index;
                            upmid.coor = offset_to_screen(curln, from);
                            from += step - back + 1;
                            dnmid.coor = offset_to_screen(curln, from);
                            delta += coord - upmid.coor;
                            uptop.role = dntop.role = grip::idle;
                            upmid.role = dnmid.role = grip::base;
                            upend.role = dnend.role = grip::idle;
                            return true;
                        }
                        else return faux;
                    };
                    from += step;
                    auto done = resx(curln);
                    if (!done)
                    {
                        auto head = batch.iter_by_id(startid);
                        auto find = [&](auto tail, auto proc)
                        {
                            auto accum = ahead ? curln.height(panel.x)
                                               : si32{0};
                            while (head != tail)
                            {
                                auto& line = proc(head);
                                from = ahead ? 0 : line.length();
                                if (resx(line))
                                {
                                    delta.y += ahead ?-accum
                                                     : accum + line.height(panel.x);
                                    return true;
                                }
                                accum += line.height(panel.x);
                            }
                            return faux;
                        };
                        if (ahead)
                        {
                            uirev = faux;
                            done = find(batch.end() - 1, [](auto& head) -> auto& { return *++head; });
                            if (!done && sctop)
                            {
                                from = si32{ 0 };
                                done = bufferbase::selection_search(dnbox, from, direction, upend.coor, dnend.coor);
                                if (done)
                                {
                                    uptop.role = dntop.role = grip::idle;
                                    upmid.role = dnmid.role = grip::idle;
                                    upend.role = dnend.role = grip::base;
                                }
                            }
                        }
                        else
                        {
                            uifwd = faux;
                            done = find(batch.begin(), [](auto& head) -> auto& { return *--head; });
                            if (!done && scend)
                            {
                                from = upbox.size().x * upbox.size().y;
                                done = bufferbase::selection_search(upbox, from, direction, uptop.coor, dntop.coor);
                                if (done)
                                {
                                    uptop.role = dntop.role = grip::base;
                                    upmid.role = dnmid.role = grip::idle;
                                    upend.role = dnend.role = grip::idle;
                                }
                            }
                        }
                    }
                    if (done)
                    {
                        if (ahead) uirev = true;
                        else       uifwd = true;
                    }
                    return done;
                };

                if (upmid.role == grip::base)
                {
                    auto init = upmid;
                    auto stop = dnmid;
                    if (init.coor.y >  stop.coor.y
                    || (init.coor.y == stop.coor.y && init.coor.x > stop.coor.x)) std::swap(init, stop);

                    auto center = selection_center(init.link, init.coor);
                    delta += center; // Always centered.
                    probe(upmid.link, init.coor);
                }
                else
                {
                    if (uptop.role == grip::base)
                    {
                        auto p1 = uptop.coor;
                        auto p2 = dntop.coor;
                        if (p1.y > p2.y || (p1.y == p2.y && p1.x > p2.x)) std::swap(p1, p2);

                        auto from = p1.x + p1.y * upbox.size().x;
                        auto done = bufferbase::selection_search(upbox, from, direction, uptop.coor, dntop.coor);
                        if (!done && ahead)
                        {
                            // Get first visible line.
                            auto fromxy = twod{ 0, batch.ancdy };
                            if (probe(batch.ancid, fromxy))
                            {
                                auto center = fromxy - selection_viewport_center();
                                delta -= center;
                            }
                        }
                        else delta = dot_00;
                    }
                    else if (upend.role == grip::base)
                    {
                        auto p1 = upend.coor;
                        auto p2 = dnend.coor;
                        if (p1.y > p2.y || (p1.y == p2.y && p1.x > p2.x)) std::swap(p1, p2);

                        auto from = p1.x + p1.y * dnbox.size().x;
                        auto done = bufferbase::selection_search(dnbox, from, direction, upend.coor, dnend.coor);
                        if (!done && !ahead)
                        {
                            // Get last visible line.
                            auto vpos =-batch.ancdy;
                            auto head = batch.iter_by_id(batch.ancid);
                            auto tail = batch.end();
                            while (head != tail)
                            {
                                auto& curln = *head++;
                                auto newpos = vpos + curln.height(panel.x);
                                if (newpos >= arena) break;
                                vpos = newpos;
                            }
                            auto& curln = *--head;
                            auto coorxy = twod{ panel.x - owner.origin.x, arena - vpos };
                            auto offset = screen_to_offset(curln, coorxy);
                            auto fromxy = offset_to_screen(curln, offset);
                            if (probe(curln.index, fromxy))
                            {
                                auto center = selection_viewport_center();
                                delta.x -= panel.x / 2 - coorxy.x - fromxy.x;
                                delta.y -= vpos - center.y;
                            }
                        }
                        else delta = dot_00;
                    }
                }

                if (upmid.role == grip::base)
                {
                    auto new_origin_x = owner.origin.x + delta.x;
                    delta.x -= new_origin_x - reset_viewport(new_origin_x, upmid.coor.x, panel.x);
                }
                else delta = dot_00;

                bufferbase::selection_update(faux);
                return delta;
            }
            // scroll_buf: Return match navigation state.
            si32 selection_button(twod delta = {}) override
            {
                auto forward_is_available = si32{};
                auto reverse_is_available = si32{};
                if (match.empty())
                {
                    forward_is_available = batch.slide - delta.y >= batch.vsize - arena ? 0 : 1 << 0;
                    reverse_is_available = batch.slide - delta.y <= 0                   ? 0 : 1 << 1;
                }
                else
                {
                    forward_is_available = uifwd ? 1 << 0 : 0;
                    reverse_is_available = uirev ? 1 << 1 : 0;
                }
                return forward_is_available | reverse_is_available;
            }
            bool selection_cancel() override
            {
                selection_drag_clear();
                return bufferbase::selection_cancel();
            }
        };
