        // term: Terminal title tracking functionality.
        struct w_tracking
        {
            term&                owner; // w_tracking: Terminal object reference.
            std::map<text, text> props;
            std::map<text, txts> stack;
            escx                 queue;

            w_tracking(term& owner)
                : owner{ owner }
            { }
            void reset()
            {
                props.clear();
                stack.clear();
                queue.clear();
                set(ansi::osc_title);
            }
            // w_tracking: Get terminal window property.
            auto& get(text const& property)
            {
                auto& utf8 = props[property];
                if (property == ansi::osc_title)
                {
                    owner.base::riseup(tier::request, e2::form::prop::ui::header, utf8);
                }
                return utf8;
            }
            // w_tracking: Set terminal window property.
            void set(text const& property, qiew txt = {})
            {
                if (txt.empty()) txt = owner.appcfg.cmd; // Deny empty titles.
                owner.target->flush();
                if (property == ansi::osc_label_title)
                {
                                  props[ansi::osc_label] = txt;
                    auto& utf8 = (props[ansi::osc_title] = txt);
                    owner.base::riseup(tier::preview, e2::form::prop::ui::header, utf8);
                }
                else
                {
                    auto& utf8 = (props[property] = txt);
                    if (property == ansi::osc_title)
                    {
                        owner.base::riseup(tier::preview, e2::form::prop::ui::header, utf8);
                    }
                }
            }
            // w_tracking: CSI n n  Device status report (DSR).
            void report(si32 n)
            {
                switch (n)
                {
                    default:
                    case 6: queue.report(owner.target->coord); break;
                    case 5: queue.add("OK");                   break;
                    case-1: queue.add("VT420");                break;
                }
                owner.answer(queue);
            }
            // w_tracking: CSI n c  Primary device attributes (DA1).
            void device(si32 n)
            {
                switch (n)
                {
                    case 0:
                    default:
                        // 61: VT Level 1 conformance
                        // 22: Color text
                        // 28: Rectangular area operations
                        // 52: Clipboard operations
                        // 10060: VT2D
                        queue.add("\x1b[?61;22;28;52;10060c");
                        break;
                }
                owner.answer(queue);
            }
            // w_tracking: Manage terminal window props (XTWINOPS).
            void manage(fifo& q)
            {
                owner.target->flush();
                static constexpr auto all_title = si32{ 0  }; // Sub commands.
                static constexpr auto label     = si32{ 1  }; // Sub commands.
                static constexpr auto title     = si32{ 2  }; // Sub commands.
                static constexpr auto set_winsz = si32{ 8  }; // Set window size in characters.
                static constexpr auto maximize  = si32{ 9  }; // Toggle maximize/restore.
                static constexpr auto full_scrn = si32{ 10 }; // Toggle fullscreen mode.
                static constexpr auto view_size = si32{ 18 }; // Report viewport size.
                static constexpr auto get_label = si32{ 20 }; // Report icon   label. (Report as OSC L label ST).
                static constexpr auto get_title = si32{ 21 }; // Report window title. (Report as OSC l title ST).
                static constexpr auto put_stack = si32{ 22 }; // Push icon label and window title to   stack.
                static constexpr auto pop_stack = si32{ 23 }; // Pop  icon label and window title from stack.
                switch (auto option = q(0))
                {
                    case maximize:
                    case full_scrn:
                        owner.window_resize(dot_00);
                        break;
                    case set_winsz:
                    {
                        auto winsz = twod{};
                        winsz.y = q(-1);
                        winsz.x = q(-1);
                        owner.window_resize(winsz);
                        break;
                    }
                    case view_size: owner.answer(queue.win_sz(owner.target->panel)); break;
                    case get_label: owner.answer(queue.osc(ansi::osc_label_report, "")); break; // Return an empty string for security reasons
                    case get_title: owner.answer(queue.osc(ansi::osc_title_report, "")); break;
                    case put_stack:
                    {
                        auto push = [&](auto const& property)
                        {
                            stack[property].push_back(props[property]);
                        };
                        switch (q(all_title))
                        {
                            case title:     push(ansi::osc_title); break;
                            case label:     push(ansi::osc_label); break;
                            case all_title: push(ansi::osc_title);
                                            push(ansi::osc_label); break;
                            default: break;
                        }
                        break;
                    }
                    case pop_stack:
                    {
                        auto pop = [&](auto const& property)
                        {
                            auto& s = stack[property];
                            if (s.size())
                            {
                                set(property, s.back());
                                s.pop_back();
                            }
                        };
                        switch (q(all_title))
                        {
                            case title:     pop(ansi::osc_title); break;
                            case label:     pop(ansi::osc_label); break;
                            case all_title: pop(ansi::osc_title);
                                            pop(ansi::osc_label); break;
                            default: break;
                        }
                        break;
                    }
                    default:
                        log("%%CSI %option%... t (XTWINOPS) is not supported", prompt::term, option);
                        break;
                }
            }
        };
