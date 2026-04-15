// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#pragma once

namespace netxs::app::vtm
{
    static constexpr auto id = "vtm";
    using ui::sptr;
    using ui::wptr;

    struct applink
    {
        text menuid{};
        text type{};
        rect square{};
        bool forced{};
        sptr applet{};
    };

    namespace events
    {
        EVENTPACK( vtm::events, ui::e2::extra::slot1 )
        {
            EVENT_XS( newapp  , applink ), // request: Create new object using specified meniid.
            EVENT_XS( apptype , applink ), // request: Ask app type.
            EVENT_XS( handoff , applink ), // general: Attach spcified intance and return sptr.
            EVENT_XS( attached, sptr    ), // anycast: Inform that the object tree is attached to the world.
            GROUP_XS( d_n_d   , sptr    ), // Drag&drop functionality. See tiling manager empty slot and pro::d_n_d.
            GROUP_XS( gate    , sptr    ),

            SUBSET_XS(d_n_d)
            {
                EVENT_XS( ask  , sptr    ),
                EVENT_XS( abort, sptr    ),
                EVENT_XS( drop , applink ),
            };
            SUBSET_XS(gate)
            {
                EVENT_XS( fullscreen, applink ), // release: Toggle fullscreen mode.
            };
        };
    };
}
