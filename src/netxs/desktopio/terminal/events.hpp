// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#pragma once

#include "../console.hpp"

namespace netxs::events::userland
{
    namespace terminal
    {
        // Find-bar search request payload.
        //   query : UTF-8 pattern to search for (empty means "re-use previous query").
        //   dir   : direction -- 1 = forward (next match), -1 = backward (previous match).
        struct find_req
        {
            text query{};
            si32 dir{ 1 };
        };
        // Find-bar count/index report payload, broadcast by the backend.
        //   query : the query that was measured (echo of the request).
        //   total : total number of matches currently found in the scrollback.
        //   index : 1-based ordinal of the current match (0 when nothing is selected).
        struct find_res
        {
            text query{};
            si32 total{ 0 };
            si32 index{ 0 };
        };

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
            GROUP_XS( find,    si32 ),

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
            SUBSET_XS( find )
            {
                // Toggle find-bar visibility. Payload semantics:
                //   -1 : flip current state
                //    0 : hide
                //    1 : show
                EVENT_XS( toggle , si32     ),
                // Broadcast find-bar visibility after it changed. Payload: 0 hidden, 1 visible.
                EVENT_XS( status , si32     ),
                // Ask the terminal backend to run a search. Emitted by the find-bar UI.
                EVENT_XS( request, find_req ),
                // Backend -> UI: result of the most recent request
                // (total match count + 1-based current-match index).
                EVENT_XS( result , find_res ),
            };
            SUBSET_XS( colors )
            {
                EVENT_XS( bg, argb ),
                EVENT_XS( fg, argb ),
            };
        };
    }
}
