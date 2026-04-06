// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#pragma once

#include "events.hpp"

namespace netxs::ui
{
    namespace terminal
    {
        namespace event_source
        {
            static constexpr auto _counter = __COUNTER__ + 1;
            static constexpr auto keyboard  = 1 << (__COUNTER__ - _counter);
            static constexpr auto mouse     = 1 << (__COUNTER__ - _counter);
            static constexpr auto focus     = 1 << (__COUNTER__ - _counter);
            static constexpr auto format    = 1 << (__COUNTER__ - _counter);
            static constexpr auto clipboard = 1 << (__COUNTER__ - _counter);
            static constexpr auto window    = 1 << (__COUNTER__ - _counter);
            static constexpr auto system    = 1 << (__COUNTER__ - _counter);
        }
        namespace events = netxs::events::userland::terminal;
        static auto event_source_map = utf::unordered_map<text, si32>
           {{ "keyboard"s,  event_source::keyboard  },
            { "mouse"s,     event_source::mouse     },
            { "focus"s,     event_source::focus     },
            { "format"s,    event_source::format    },
            { "clipboard"s, event_source::clipboard },
            { "window"s,    event_source::window    },
            { "system"s,    event_source::system    }};
    }
}
