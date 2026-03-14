// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "netxs/apps/desk.hpp"
#include "vtm.hpp"
#include "netxs/apps/tile.hpp"

using namespace netxs;
using namespace netxs::app;

namespace
{
    auto verify_standalone_tile_shutdown() -> bool
    {
        auto tile_root = ui::cake::ctor();
        auto shutdown_seen = false;
        auto quit_seen = false;

        tile_root->LISTEN(tier::general, e2::shutdown, reason)
        {
            shutdown_seen = !reason.empty();
        };
        tile_root->LISTEN(tier::release, e2::form::proceed::quit::one, fast)
        {
            quit_seen = fast;
        };

        app::tile::close_tile_session(*tile_root, "regression standalone");
        return shutdown_seen && !quit_seen;
    }

    auto verify_embedded_tile_close_path() -> bool
    {
        auto desktop_root = ui::cake::ctor();
        auto tile_root = desktop_root->attach(ui::cake::ctor());
        auto shutdown_seen = false;
        auto quit_fast_seen = false;

        tile_root->LISTEN(tier::general, e2::config::creator, world_ptr)
        {
            world_ptr = tile_root;
        };
        tile_root->LISTEN(tier::general, e2::shutdown, reason)
        {
            shutdown_seen = !reason.empty();
        };
        desktop_root->LISTEN(tier::release, e2::form::proceed::quit::one, fast)
        {
            quit_fast_seen = fast;
        };

        app::tile::close_tile_session(*tile_root, "regression embedded");
        return !shutdown_seen && quit_fast_seen;
    }
}

auto main() -> int
{
    if (!verify_standalone_tile_shutdown()) return 1;
    if (!verify_embedded_tile_close_path()) return 2;
    return 0;
}
