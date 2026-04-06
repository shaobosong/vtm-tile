        // term: Keyboard focus tracking functionality.
        struct f_tracking
        {
            using prot = input::focus::prot;

            term& owner; // f_tracking: Terminal object reference.
            hook  token; // f_tracking: Subscription token.
            prot  encod; // f_tracking: Focus encoding mode.
            bool  state; // f_tracking: Current focus state.

            f_tracking(term& owner)
                : owner{ owner },
                  encod{ prot::w32 }
            {
                owner.LISTEN(tier::release, e2::form::state::focus::count, count, token)
                {
                    auto focused = !!count;
                    if (std::exchange(state, focused) != state)
                    {
                        owner.ipccon.focus(focused, encod);
                        if (!focused && owner.ime_on) owner.ime_on = faux;
                    }
                };
                state = owner.base::signal(tier::request, e2::form::state::focus::count);
            }

            operator bool () { return state; }
            void set(bool enable)
            {
                encod = enable ? prot::dec : prot::w32;
            }
        };
