// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#pragma once

#include "../console.hpp"

namespace netxs::events::userland
{
    namespace terminal
    {
        EVENTPACK( terminal::events, ui::e2::extra::slot5 )
        {
            EVENT_XS( io_log,  si32 ),
            EVENT_XS( selmod,  si32 ),
            EVENT_XS( onesht,  si32 ),
            EVENT_XS( selalt,  si32 ),
            EVENT_XS( rawkbd,  si32 ),
            GROUP_XS( toggle,  si32 ),
            GROUP_XS( preview, si32 ),
            GROUP_XS( release, si32 ),
            GROUP_XS( colors,  argb ),
            GROUP_XS( layout,  si32 ),
            GROUP_XS( search,  input::hids ),

            SUBSET_XS( toggle )
            {
                EVENT_XS( cwdsync, si32 ), // preview: Request to toggle cwdsync.
            };
            SUBSET_XS( preview )
            {
                EVENT_XS( cwdsync, si32 ),
            };
            SUBSET_XS( release )
            {
                EVENT_XS( cwdsync, si32 ),
            };
            SUBSET_XS( layout )
            {
                EVENT_XS( align , si32 ),
                EVENT_XS( wrapln, si32 ),
            };
            SUBSET_XS( search )
            {
                EVENT_XS( forward, input::hids ),
                EVENT_XS( reverse, input::hids ),
                EVENT_XS( status , si32        ),
            };
            SUBSET_XS( colors )
            {
                EVENT_XS( bg, argb ),
                EVENT_XS( fg, argb ),
            };
        };
    }
}
