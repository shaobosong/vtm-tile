        // term: VT-buffer status.
        struct term_state
        {
            enum type
            {
                empty,
                lines,
                block,
            };

            si32 size{}; // term_state: Terminal scrollback current size.
            si32 peak{}; // term_state: Terminal scrollback limit.
            si32 step{}; // term_state: Terminal scrollback increase step.
            si32 mxsz{}; // term_state: Terminal scrollback increase limit.
            twod area{}; // term_state: Terminal viewport size.
            escx data{}; // term_state: Status string.
            type mode{}; // term_state: Selection mode.
            twod coor{}; // term_state: Selection coor.
            ui64 body{}; // term_state: Selection rough volume.
            ui64 hash{}; // term_state: Selection update indicator.

            template<class BufferBase>
            auto update(BufferBase const& scroll)
            {
                if (scroll.update_status(*this))
                {
                    data.clear();
                    if (hash) data.scp();
                    data.jet(bias::right);
                    auto total = std::max(mxsz, peak);
                    //todo optimize?
                    if (total % 1000)
                    {
                        data.add(size, "/"sv, total);
                    }
                    else // Apply decimal prefixes to terminal scrollback size.
                    {
                        if (size % 1000) data.add(size);
                        else
                        {
                            if (!(size % 1000000)) data.add(size / 1000000, 'M');
                            else                   data.add(size / 1000, 'K');
                        }
                        data.add("/"sv);
                        if (!(total % 1000000)) data.add(total / 1000000, 'M');
                        else                    data.add(total / 1000, 'K');
                    }
                    //if (mxsz && step && size != mxsz) data.add("+", step);
                    data.add(" ", area.x, ":", area.y);
                    if (hash)
                    {
                        data.rcp().jet(bias::left);
                        if (mode == type::block)
                        {
                            data.add(coor.x, ":", coor.y, " ");
                        }
                        else
                        {
                            data.add(coor.y, coor.y == 1 ? " row " : " rows ");
                                 if (body == 1) data.add("1 cell ");
                            else if (body <100) data.add     (body, " cells ");
                            else                data.add("~", body, " cells ");
                        }
                    }
                    return true;
                }
                else return faux;
            }
        };
