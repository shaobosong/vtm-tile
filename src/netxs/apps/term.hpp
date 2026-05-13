// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#pragma once

// term: Teletype Console.
namespace netxs::app::teletype
{
    static constexpr auto id = "teletype";
    static constexpr auto name = "Teletype Console";
}
// term: Terminal Console.
namespace netxs::app::terminal
{
    static constexpr auto id = "terminal";
    static constexpr auto name = "Terminal Console";

    namespace attr
    {
        static constexpr auto cwdsync       = "/config/terminal/cwdsync";
        static constexpr auto borders       = "/config/terminal/border";
        static constexpr auto confirmclose  = "/config/terminal/confirm_close";
    }

    auto ui_term_events = [](ui::term& boss, eccc& appcfg, bool confirm_close, std::weak_ptr<ui::base> confirm_target)
    {
        boss.LISTEN(tier::anycast, e2::form::proceed::quit::any, fast, -, (confirm_close, confirm_target))
        {
            if (confirm_close && !fast)
            {
                if (auto target = confirm_target.lock())
                {
                    app::shared::show_close_confirmation(*target, [confirm_target]
                    {
                        if (auto w = confirm_target.lock())
                        {
                            w->base::riseup(tier::release, e2::form::proceed::quit::one, true);
                        }
                    });
                }
            }
            else
            {
                boss.base::signal(tier::preview, e2::form::proceed::quit::one, fast);
            }
        };
        boss.LISTEN(tier::preview, e2::form::proceed::quit::one, fast)
        {
            boss.close(fast);
        };
        boss.LISTEN(tier::release, e2::form::upon::started, root_ptr, -, (appcfg))
        {
            if (!root_ptr) return; // root_ptr is empty when d_n_d.
            // Wait until the viewport is at least 2 rows tall before forking
            // the shell. dtvt applets receive a spawn_size snapshot before
            // launch(), but the parent tile may still be reflowing and pass a
            // 1-row bootstrap; the real size arrives shortly after via
            // syswinsz. Forking the shell at the bootstrap width strands its
            // first prompt line until a later deform happens to refresh it.
            auto enqueue_start = [&boss, appcfg]() mutable
            {
                boss.base::enqueue([&boss, appcfg, backup = boss.This()](auto& /*widget*/) mutable
                {
                    boss.start_term(appcfg);
                    backup.reset(); // Backup should dtored under the lock.
                });
            };
            if (boss.base::size().y > 1)
            {
                enqueue_start();
                return;
            }
            auto& startup_hook = boss.base::field(hook{});
            boss.LISTEN(tier::release, e2::area, new_area, startup_hook, (enqueue_start))
            {
                if (new_area.size.y < 2) return;
                enqueue_start();
                boss.base::unfield(startup_hook);
            };
        };
        boss.LISTEN(tier::anycast, e2::form::upon::started, root_ptr)
        {
            boss.base::signal(tier::release, e2::form::upon::started, root_ptr);
        };
    };
    auto build_teletype = [](eccc appcfg, settings& config)
    {
        auto confirm_close = config.settings::take(attr::confirmclose, faux);
        auto shadower = skin::color(tone::shadower);
        auto window = ui::cake::ctor()
            ->plugin<pro::focus>()
            ->invoke([&, confirm_close](auto& boss)
            {
                if (!confirm_close) app::shared::closing_on_quit(boss);
            });
        window//->plugin<pro::track>()
            //->plugin<pro::acryl>()
            ->plugin<pro::cache>();
        auto layers = window->attach(ui::cake::ctor())
                            ->shader(cell::shaders::fuse(shadower))
                            ->limits(dot_11);
        auto scroll = layers->attach(ui::rail::ctor())
                            ->limits({ 10,1 }); // mc crashes when window is too small
        if (appcfg.cmd.empty()) appcfg.cmd = os::env::shell();//todo revise + " -i";
        auto term = scroll->attach(ui::term::ctor())
            ->plugin<pro::focus>(pro::focus::mode::focused)
            ->invoke([&, confirm_close, window_shadow = std::weak_ptr<ui::base>(window)](auto& boss)
            {
                ui_term_events(boss, appcfg, confirm_close, window_shadow);
            });
        layers->attach(app::shared::scroll_bars(scroll));
        return window;
    };
    // Shared per-bar state for the find-bar overlay.
    // Defined at namespace scope to avoid MSVC lambda-invoker type-identity
    // duplication (C3889) when the struct is used as a parameter of a
    // captured lambda inside build_terminal.
    struct find_state
    {
        text query;           // current query (UTF-8 bytes)
        si32 caret_cp{ 0 };   // caret position in codepoints
        si32 scroll_cp{ 0 };  // first visible codepoint (horizontal scroll)
        bool visible{ faux };
        si32 hover{ 0 };      // hov_none / hov_input / hov_btn_*
        si32 dir{ 1 };        // active navigation direction: -1=up, +1=down (default: down)
        si32 total{ 0 };      // total matches reported by backend (0..counter_max)
        si32 index{ 0 };      // 1-based current match index (0 = no selection)
    };

    auto build_terminal = [](eccc appcfg, settings& config)
    {
        auto border = std::max(0, config.settings::take(attr::borders, 0));
        auto confirm_close = config.settings::take(attr::confirmclose, faux);
        auto borders = dent{ border, border, 0, 0 };
        auto window = ui::cake::ctor();
        auto& window_clr = window->base::field(skin::color(tone::window_clr));
        auto& is_focused = window->base::field(faux);
        window->plugin<pro::focus>()
            //->plugin<pro::track>()
            //->plugin<pro::acryl>()
            ->plugin<pro::cache>()
            ->invoke([&](auto& boss)
            {
                boss.LISTEN(tier::release, e2::form::state::focus::count, count)
                {
                    if (std::exchange(is_focused, !!count) != is_focused)
                    {
                        boss.base::deface(); // Trigger to update pro::cache.
                        window_clr = is_focused ? skin::color(tone::winfocus)
                                                : skin::color(tone::window_clr);
                    }
                };
            });

        auto object = window->attach(ui::fork::ctor(axis::Y));
        auto term_stat_area = object->attach(slot::_2, ui::fork::ctor(axis::Y))
            ->setpad(borders)
            ->invoke([&](auto& boss)
            {
                if (borders)
                boss.LISTEN(tier::release, e2::render::background::any, parent_canvas, -, (borders)) // Shade left/right borders.
                {
                    auto full = parent_canvas.full();
                    parent_canvas.cage(full, borders, [&](cell& c){ c.fuse(window_clr); });
                };
            });
        auto layers = term_stat_area->attach(slot::_1, ui::cake::ctor())
                                    ->limits(dot_11);
        auto scroll = layers->attach(ui::rail::ctor()->smooth(faux));
        auto min_size = twod{ 1,1 }; // Was 12,1 to avoid mc crashes; lowered so menubar can shrink to allow ctrllist close button to track right edge.
        auto max_size = -dot_11;
        scroll->limits(min_size, max_size)
            ->invoke([](auto& boss)
            {
                boss.LISTEN(tier::preview, e2::form::prop::window::size, new_size)
                {
                    // Axis x/y (see XTWINOPS):
                    //   -1 -- preserve
                    //    0 -- maximize (toggle)
                    if (new_size == dot_00) // Toggle fullscreen terminal (only if it is focused by someone).
                    {
                        auto gear_id_list = boss.base::riseup(tier::request, e2::form::state::keybd::enlist);
                        for (auto gear_id : gear_id_list)
                        {
                            if (auto gear_ptr = boss.base::template getref<hids>(gear_id)) //todo Apple clang requires template
                            {
                                auto& gear = *gear_ptr;
                                boss.base::riseup(tier::preview, e2::form::size::enlarge::fullscreen, gear);
                                break;
                            }
                        }
                    }
                    else if (boss.base::size() != new_size)
                    {
                        auto panel = boss.base::size();
                        new_size = new_size.less(dot_11, panel, std::max(dot_11, new_size));
                        auto warp = rect{ dot_00, new_size } - rect{ dot_00, panel };
                        boss.base::locked = faux; // Unlock resizing.
                        boss.base::resize(new_size);
                        boss.base::locked = true; // Lock resizing until reflow is complete.
                        boss.base::riseup(tier::preview, e2::form::layout::swarp, warp);
                    }
                };
                boss.LISTEN(tier::release, e2::area, new_area)
                {
                    boss.base::locked = faux; // Unlock resizing.
                };
            });

        if (appcfg.cmd.empty()) appcfg.cmd = os::env::shell();//todo revise + " -i";
        auto terminal_context = config.settings::push_context("/config/terminal/");
        auto term = scroll->attach(ui::term::ctor())
            ->plugin<pro::focus>(pro::focus::mode::focused)
            ->invoke([&](auto& boss)
            {
                auto& cwd_commands = boss.base::property("terminal.cwd_commands", config.settings::take(attr::cwdsync, ""s));
                auto& cwd_sync = boss.base::property("terminal.cwd_sync", 0);
                auto& cwd_path = boss.base::property("terminal.cwd_path", os::fs::path{});
                boss.LISTEN(tier::preview, ui::terminal::events::toggle::cwdsync, state)
                {
                    boss.base::signal(tier::anycast, ui::terminal::events::preview::cwdsync, state);
                };
                boss.LISTEN(tier::anycast, ui::terminal::events::preview::cwdsync, state)
                {
                    if (cwd_sync != state)
                    {
                        cwd_sync = state;
                        boss.base::signal(tier::anycast, ui::terminal::events::release::cwdsync, state);
                        if (cwd_sync)
                        {
                            auto cmd = cwd_commands;
                            utf::replace_all(cmd, "$P", ".");
                            boss.data_out(cmd); // Trigger command prompt reprint.
                        }
                    }
                };
                boss.LISTEN(tier::preview, e2::form::prop::cwd, path)
                {
                    // Always pass the riseup through so outer consumers (e.g., the tile
                    // applet's per-pane cwd tracker that lets cwd=true items launch from
                    // the focused pane's directory) can observe the OSC9;9 stream. The
                    // cwd_sync flag still gates the inner term's own bookkeeping and the
                    // command echo loop at the anycast listener below.
                    boss.bell::passover();
                    if (cwd_sync)
                    {
                        cwd_path = path;
                    }
                };
                if (cwd_commands.size())
                {
                    boss.LISTEN(tier::anycast, e2::form::prop::cwd, path)
                    {
                        if (cwd_sync && path.size() && cwd_path != path)
                        {
                            cwd_path = path;
                            auto cwd = cwd_path.string();
                            if (cwd.find(' ') != text::npos) cwd = '\"' + cwd + '\"';
                            auto cmd = cwd_commands;
                            utf::replace_all(cmd, "$P", cwd);
                            boss.data_out(cmd);
                        }
                    };
                }
            });

        // ============================================================
        //  Find-bar overlay
        // ============================================================
        //
        //  A self-contained search UI that floats above the terminal.
        //  Entirely redrawn on every frame from its own state -- it does
        //  not compose a tree of sub-widgets.  Layout:
        //
        //           row 0: (1-row gap above the bar)
        //           row 1:     ╭─ Find ───────────────────────────────────╮
        //           row 2:     │[query_text__________][999+/999+][↑][↓][×]│
        //           row 3:     ╰──────────────────────────────────────────╯
        //
        //  In the ASCII-art above, each [...] denotes one control whose
        //  visible region already includes 1 cell of padding on each side
        //  (the `[` and `]` are those padding cells).  Controls sit flush
        //  against each other and against the frame borders; the visible
        //  spaces between them come from those built-in paddings.
        //
        //  * Rounded corners  : ╭╮╰╯
        //  * Straight edges   : ─  │
        //  * Input field      : underlined row (cell underline attribute, no
        //                       `_` glyphs); scrolls horizontally when the
        //                       query is longer than the field
        //  * Match counter    : right-aligned "NNN/NNN" (≤999) or "999+/999+"
        //                       (overflow), shown as "000/000" when the
        //                       query is empty.
        //  * Up/Down buttons  : radio-style direction selector. Exactly
        //                       one of them is "active" at any time; the
        //                       active one gets a distinct background.
        //                       The active direction is the one Enter
        //                       (with no modifier) will move in.
        //  * Close button     : × dismisses the bar.
        //  * Custom cursor    : drawn by the bar itself, terminal cursor
        //                       is suppressed in every cell the bar owns
        //                       (see text_cursor::none fills below)
        //
        //  Events
        //  ------
        //  tier::anycast terminal::events::find::toggle  -- show/hide/flip
        //  tier::anycast terminal::events::find::request -- ask the backend
        //                                                    to navigate to
        //                                                    the next match
        //  tier::anycast terminal::events::find::result  -- backend -> UI,
        //                                                    counter update
        //  tier::anycast terminal::events::find::status  -- broadcast state
        //
        //  Shortcut summary while the bar is visible
        //  -----------------------------------------
        //    <printable>       append to query (codepoint-aware)
        //    Backspace         delete previous codepoint
        //    Left / Right      move caret one codepoint
        //    Home / End        jump to caret start / end
        //    Tab               toggle active direction (↑ ↔ ↓) and
        //                      re-seed the search from the new edge;
        //                      the direction button highlight updates
        //                      immediately
        //    Enter             navigate to next match in the active
        //                      direction (Shift+Enter = opposite dir,
        //                      subject to modifier reporting)
        //    Up / Down         navigate to previous / next match in
        //                      absolute direction (portable across
        //                      GUI and pty/nested input protocols)
        //    Esc               close the bar
        //    any other key     swallowed (does not leak to the shell)
        {
            // Box-drawing characters (single-byte-width, rounded corners).
            static constexpr auto ch_tl    = "╭";
            static constexpr auto ch_tr    = "╮";
            static constexpr auto ch_bl    = "╰";
            static constexpr auto ch_br    = "╯";
            static constexpr auto ch_hz    = "─";
            static constexpr auto ch_vt    = "│";
            static constexpr auto ch_sp    = " ";

            // Geometry.  The bar adapts to the available width with a hard
            // minimum below which it refuses to render.  At full width it
            // shows: borders + input(21) + counter(11) + ↑↓×(9) = 44 cells.
            // When the host terminal is narrow, controls are dropped in
            // reverse priority order so the most important ones survive
            // (see `layout_of`).
            static constexpr auto bar_rows     = si32{ 3 };       // frame height
            static constexpr auto bar_cols     = si32{ 44 };      // preferred (max) outer width including corners
            static constexpr auto min_input_w  = si32{ 4 };       // mandatory minimum input field cells
            // Smallest bar that still carries the most-important controls
            // (close button + 4-cell input + borders + left pad).
            //   borders(2) + pad_l(1) + input(4) + close_btn(3) = 10
            static constexpr auto min_bar_cols = si32{ 10 };
            static constexpr auto gap_rows     = si32{ 1 };       // space above the bar
            // Inner layout. Every control has 1 cell of padding on each side
            // (the `[` and `]` in the ASCII-art above). Controls sit flush
            // against each other and against the frame borders.  As of the
            // gap-tightening change the input group drops its 1-cell right
            // pad (the counter's left pad already provides visible
            // separation), shaving one cell off the gap between the counter
            // and the in-input clear button.
            //   border + pad   = 2 cells on the left, 0 on the right
            //   input group    = 1 + 21 + 0 = 22 cells (left pad + 21-cell field)
            //   counter group  = 1 +  9 + 1 = 11 cells   ("999+/999+" = 9 glyphs, right-aligned)
            //   each button    = 1 +  1 + 1 =  3 cells
            //   buttons area   = 3 ×  3     =  9 cells
            //   total          = 1 + 22 + 11 + 9 + 1 = 44  (1 border cell on each side)
            static constexpr auto counter_cells      = si32{ 11 };   // including surrounding pads
            static constexpr auto counter_digits     = si32{ 3 };    // per side, 3-digit zero-pad (max 999)
            static constexpr auto counter_max        = si32{ 999 };  // overflow threshold (show "999+")
            static constexpr auto counter_text_width = si32{ 9 };    // right-aligned text field width
            static constexpr auto btn_area_cells     = si32{ 9 };    // 3 buttons × 3 cells
            static constexpr auto btn_width          = si32{ 3 };
            // Left padding inside the frame (the cell just after `│`).  No
            // right pad: the input strip extends right up to the counter's
            // own left pad, which is what visually separates the two.
            static constexpr auto input_pad_l = si32{ 1 };

            // Colour palette (Tokyo-Night-ish, matches close-confirm dialog).
            static constexpr auto col_bg     = argb{ 0x00000000 };
            static constexpr auto col_hov_bg = argb{ 0xff414868 }; // hover background (brighter than col_bg)
            static constexpr auto col_act_bg = argb{ 0xff3d59a1 }; // active direction-button background (distinct blue)
            static constexpr auto col_brd    = argb{ 0xff7aa2f7 };
            static constexpr auto col_label  = argb{ 0xff7aa2f7 };
            static constexpr auto col_text   = argb{ 0xffc0caf5 };
            static constexpr auto col_uline  = argb{ 0xff565f89 };
            static constexpr auto col_btn    = argb{ 0xffa9b1d6 };
            static constexpr auto col_btn_x  = argb{ 0xfff7768e }; // close button
            static constexpr auto col_count  = argb{ 0xff9ece6a }; // match counter text (green)
            static constexpr auto col_caret  = argb{ 0xffe0af68 };

            // Hover zones.
            static constexpr auto hov_none      = si32{ 0 };
            static constexpr auto hov_input     = si32{ 1 };
            static constexpr auto hov_btn_up    = si32{ 2 };
            static constexpr auto hov_btn_dn    = si32{ 3 };
            static constexpr auto hov_btn_x     = si32{ 4 };
            static constexpr auto hov_btn_clear = si32{ 5 }; // clear-query button (inside input area)

            // Navigation direction. Values match terminal::find_req::dir:
            //   dir_up   = -1  (Enter moves backward, i.e. up/toward scrollback start)
            //   dir_down = +1  (Enter moves forward, i.e. down/toward scrollback end)
            static constexpr auto dir_up     = si32{ -1 };
            static constexpr auto dir_down   = si32{  1 };

            auto state_ptr = ptr::shared<find_state>();
            auto kbd_hook = ptr::shared<hook>();
            auto pending_unhook = ptr::shared(faux);

            // Root overlay on the layers stack.
            auto overlay = layers->attach(ui::cake::ctor());
            overlay->base::hidden = true;

            // Positioner: fork::Y, slot_1 is (gap+bar) fixed height, slot_2
            // absorbs everything else so the bar stays anchored to the top.
            auto positioner = overlay->attach(ui::fork::ctor(axis::Y));
            auto top_band = positioner->attach(slot::_1, ui::fork::ctor(axis::Y))
                ->limits({ -1, gap_rows + bar_rows }, { -1, gap_rows + bar_rows });
            // Spacer for gap_rows.
            top_band->attach(slot::_1, ui::mock::ctor())
                ->limits({ -1, gap_rows }, { -1, gap_rows });
            // Horizontal placement: right-aligned with a 2-cell margin to
            // the right edge.  Layout = [left filler (stretch)] [bar (fixed)]
            // [right filler (fixed 2)].
            auto bar_row = top_band->attach(slot::_2, ui::fork::ctor(axis::X))
                ->limits({ -1, bar_rows }, { -1, bar_rows });
            // Left filler absorbs all slack.
            bar_row->attach(slot::_1, ui::mock::ctor());
            // Inner fork: bar (fixed width) + 2-cell right margin.
            auto right_split = bar_row->attach(slot::_2, ui::fork::ctor(axis::X));
            auto bar = right_split->attach(slot::_1, ui::mock::ctor())
                ->limits({ min_bar_cols, bar_rows }, { bar_cols, bar_rows });
            right_split->attach(slot::_2, ui::mock::ctor())
                ->limits({ 2, -1 }, { 2, -1 });

            // Helper: compute layout of the inner input row.  Returns the
            // cell ranges for each interactive region of the bar plus
            // visibility flags for optional controls.  At full width the
            // bar is drawn left-to-right as:
            //   │ [input] [counter] [↑] [↓] [×] │
            // Each [...] group carries its own 1-cell padding on both sides.
            //
            // Responsive degradation order (least important dropped first):
            //   1. Direction buttons (↑↓ pair, 6 cells).
            //   2. Counter group (11 cells).
            //   3. In-input clear button capacity (3 input cells).
            //   4. Input width down to `min_input_w` (4 cells).
            // The close button (×, 3 cells), borders, and left pad are
            // always preserved as long as the bar is rendered at all.
            // When `outer_w < min_bar_cols` the layout is invalid and
            // `valid` is set to false (caller must skip rendering).
            struct bar_layout
            {
                bool valid;         // outer_w >= min_bar_cols
                bool show_dir;      // ↑ and ↓ buttons rendered
                bool show_counter;  // counter group rendered
                bool clear_fits;    // input is wide enough to host the clear button (>= min_input_w + btn_width)
                si32 input_x0;      // first input cell (local x)
                si32 input_x1;      // last  input cell (local x)
                si32 input_w;       // width in cells
                si32 count_x0;      // first counter cell (pad included), 0 when !show_counter
                si32 count_x1;      // last  counter cell (pad included), 0 when !show_counter
                si32 count_text_x0; // first cell that holds the 9-char right-aligned counter text
                si32 btn_up_x;      // column of "↑" glyph (center of 3-cell button), 0 when !show_dir
                si32 btn_dn_x;      // column of "↓" glyph, 0 when !show_dir
                si32 btn_x_x;       // column of "×" glyph (close button, right of bar)
                si32 btn_clear_x;   // column of "×" glyph (clear-query button, inside input area)
            };
            auto layout_of = [](si32 outer_w) -> bar_layout
            {
                auto L = bar_layout{};
                L.valid = outer_w >= min_bar_cols;
                if (!L.valid) return L;
                // Mandatory cells: 2 borders + 1 left pad + 3 close-btn = 6.
                // Remaining budget feeds (in priority of being kept):
                //   input (>= min_input_w) > clear-button capacity (3) >
                //   counter (11) > dir-pair (6).
                // Dropping order is the reverse: dir, counter, clear, then
                // shrink input (never below min_input_w).
                auto avail = outer_w - 6;          // for [input + counter? + dir?]
                auto input_w = min_input_w;        // mandatory floor
                auto remaining = avail - input_w;  // >= 0 (since outer_w >= min_bar_cols)
                // 1. Reserve 3 cells of clear-button capacity inside input.
                if (remaining >= btn_width) { input_w += btn_width; remaining -= btn_width; }
                // 2. Add counter (11 cells, fixed).
                L.show_counter = remaining >= counter_cells;
                if (L.show_counter) remaining -= counter_cells;
                // 3. Add direction-button pair (6 cells, fixed).  Direction
                // buttons rank below the counter in the priority order, so
                // they only appear when the counter has already been kept.
                L.show_dir = L.show_counter && remaining >= 2 * btn_width;
                if (L.show_dir) remaining -= 2 * btn_width;
                // 4. Distribute leftover to input growth (no upper cap is
                // needed: parent fork already caps outer_w at bar_cols=44).
                input_w += remaining;
                L.clear_fits = input_w >= min_input_w + btn_width;
                // Lay out left-to-right inside the frame.
                //   col 0       : │ (left border)
                //   col 1       : pad
                //   col 2 ..    : input field (input_w cells)
                //   then        : counter group (counter_cells cells), if shown
                //   then        : ↑↓ buttons (3+3 cells), if shown
                //   col W-4..W-2: × close button (3 cells)
                //   col W-1     : │ (right border)
                L.input_x0 = 1 + input_pad_l;                          // 2
                L.input_x1 = L.input_x0 + input_w - 1;
                L.input_w  = input_w;
                if (L.show_counter)
                {
                    L.count_x0      = L.input_x1 + 1;
                    L.count_x1      = L.count_x0 + counter_cells - 1;
                    L.count_text_x0 = L.count_x0 + 1;                  // skip 1-cell left pad
                }
                else
                {
                    L.count_x0 = 0;
                    L.count_x1 = 0;
                    L.count_text_x0 = 0;
                }
                L.btn_x_x  = outer_w - 3;                              // center of 3-cell close button (spans W-4..W-2; W-1 is │)
                if (L.show_dir)
                {
                    L.btn_dn_x = L.btn_x_x  - btn_width;
                    L.btn_up_x = L.btn_dn_x - btn_width;
                }
                else
                {
                    L.btn_up_x = 0;
                    L.btn_dn_x = 0;
                }
                // Clear-query button: rightmost 3 cells of the input area.
                // Center column = input_x1 - 1 (spans input_x1-2 .. input_x1).
                L.btn_clear_x = L.input_x1 - 1;
                return L;
            };

            // Slice UTF-8 `s` by codepoint range [cp_from, cp_from + cp_count).
            auto slice_cp = [](view s, si32 cp_from, si32 cp_count) -> text
            {
                if (cp_count <= 0 || s.empty()) return {};
                auto i = size_t{ 0 };
                auto cp = si32{ 0 };
                while (i < s.size() && cp < cp_from)
                {
                    ++i;
                    while (i < s.size() && ((byte)s[i] & 0xC0) == 0x80) ++i;
                    ++cp;
                }
                auto start = i;
                auto taken = si32{ 0 };
                while (i < s.size() && taken < cp_count)
                {
                    ++i;
                    while (i < s.size() && ((byte)s[i] & 0xC0) == 0x80) ++i;
                    ++taken;
                }
                return text{ s.substr(start, i - start) };
            };
            // Byte offset for codepoint index cp in s.
            auto byte_of_cp = [](view s, si32 cp) -> size_t
            {
                if (cp <= 0) return 0;
                auto i = size_t{ 0 };
                auto n = si32{ 0 };
                while (i < s.size() && n < cp)
                {
                    ++i;
                    while (i < s.size() && ((byte)s[i] & 0xC0) == 0x80) ++i;
                    ++n;
                }
                return i;
            };
            // Codepoint count of s.
            auto cp_len = [](view s) -> si32
            {
                auto n = si32{ 0 };
                for (auto c : s) n += ((byte)c & 0xC0) != 0x80;
                return n;
            };

            // Format the match counter as a right-aligned string within a
            // counter_text_width (9) cell field.  When a value exceeds
            // counter_max (999) it is shown as "999+" (4 chars); otherwise
            // it is zero-padded to counter_digits (3) chars.
            // Three possible outputs (left-aligned, total width = 9):
            //   "001/099  "  both sides ≤ 999  (inner width 7, 2 trailing spaces)
            //   "892/999+ "  only total > 999  (inner width 8, 1 trailing space)
            //   "999+/999+"  both sides > 999  (inner width 9, no padding)
            auto format_counter = [](si32 index, si32 total) -> text
            {
                auto side = [](si32 v) -> text
                {
                    if (v < 0) v = 0;
                    if (v > counter_max) return "999+";
                    auto buf = std::to_string(v);
                    while ((si32)buf.size() < counter_digits) buf.insert(buf.begin(), '0');
                    return buf;
                };
                auto ix_str = side(index);
                auto tt_str = side(total);
                auto inner  = ix_str + '/' + tt_str;
                // Left-align: pad with spaces on the right to reach counter_text_width.
                auto pad = counter_text_width - (si32)inner.size();
                auto s   = inner;
                s.reserve(counter_text_width);
                while (pad-- > 0) s += ' ';
                return s;
            };

            // Render the bar onto parent_canvas at the given outer rect.
            auto term_weak_for_render = std::weak_ptr<ui::term>(term);
            auto render_bar = [layout_of, slice_cp, cp_len, format_counter, term_weak_for_render](auto& canvas, rect box, find_state& st)
            {
                if (box.size.x < min_bar_cols || box.size.y < bar_rows) return;
                auto L = layout_of(box.size.x);
                if (!L.valid) return;
                auto x0 = box.coor.x;
                auto y0 = box.coor.y;
                auto w  = box.size.x;

                // Reset every attribute in the bar rect so style bits
                // (italic/bold/underline/blink/reverse/strike/overline,
                // hyperlink, pict refs, grapheme remnants) from the underlying
                // terminal content cannot bleed through the find-bar overlay.
                canvas.fill(box, [](cell& c) { c.wipe(); });

                // Frame background = terminal's current default bg color, so
                // the rounded-corner frame blends with whatever the running
                // shell has set via SGR.  Fallback to col_bg if the terminal
                // isn't reachable (shouldn't happen while the bar is alive).
                auto frame_bg = col_bg;
                if (auto t = term_weak_for_render.lock())
                {
                    frame_bg = t->get_color().bgc();
                }

                // Fill interior of the middle row only (bg + suppress cursor
                // bleed).  The frame cells (top/bottom rows and the two side
                // verticals) keep a transparent background so the content
                // beneath the bar shows through around the rounded corners.
                canvas.fill(rect{{ x0 + 1, y0 + 1 }, { w - 2, 1 }}, [](cell& c)
                {
                    c.bgc(col_bg).fgc(col_text).txt(ch_sp).cur(text_cursor::none);
                });

                // Top border: ╭─ Find ─── ... ─╮
                auto top_y = y0;
                canvas.fill(rect{{ x0,         top_y }, { 1, 1 }},
                    [frame_bg](cell& c){ c.bgc(frame_bg).fgc(col_brd).txt(ch_tl).cur(text_cursor::none); });
                canvas.fill(rect{{ x0 + w - 1, top_y }, { 1, 1 }},
                    [frame_bg](cell& c){ c.bgc(frame_bg).fgc(col_brd).txt(ch_tr).cur(text_cursor::none); });
                canvas.fill(rect{{ x0 + 1,     top_y }, { w - 2, 1 }},
                    [frame_bg](cell& c){ c.bgc(frame_bg).fgc(col_brd).txt(ch_hz).cur(text_cursor::none); });
                // Embed " Find " label at x0+2.
                auto label = " Search "sv;
                auto lx = x0 + 2;
                for (auto i = size_t{ 0 }; i < label.size() && lx < x0 + w - 2; ++i, ++lx)
                {
                    auto ch = text{ label[i] };
                    canvas.fill(rect{{ lx, top_y }, { 1, 1 }},
                        [&ch, frame_bg](cell& c){ c.bgc(frame_bg).fgc(col_label).txt(ch).cur(text_cursor::none); });
                }

                // Bottom border: ╰───────────╯
                auto bot_y = y0 + bar_rows - 1;
                canvas.fill(rect{{ x0,         bot_y }, { 1, 1 }},
                    [frame_bg](cell& c){ c.bgc(frame_bg).fgc(col_brd).txt(ch_bl).cur(text_cursor::none); });
                canvas.fill(rect{{ x0 + w - 1, bot_y }, { 1, 1 }},
                    [frame_bg](cell& c){ c.bgc(frame_bg).fgc(col_brd).txt(ch_br).cur(text_cursor::none); });
                canvas.fill(rect{{ x0 + 1,     bot_y }, { w - 2, 1 }},
                    [frame_bg](cell& c){ c.bgc(frame_bg).fgc(col_brd).txt(ch_hz).cur(text_cursor::none); });

                // Side borders on the middle row.
                auto mid_y = y0 + 1;
                canvas.fill(rect{{ x0,         mid_y }, { 1, 1 }},
                    [frame_bg](cell& c){ c.bgc(frame_bg).fgc(col_brd).txt(ch_vt).cur(text_cursor::none); });
                canvas.fill(rect{{ x0 + w - 1, mid_y }, { 1, 1 }},
                    [frame_bg](cell& c){ c.bgc(frame_bg).fgc(col_brd).txt(ch_vt).cur(text_cursor::none); });

                // Input underline area (row mid_y, cols [input_x0, input_x1]).
                // The whole input span (including the 3-cell clear-query button
                // when visible) is rendered as a single underlined strip so the
                // clear button looks visually connected to the input field.
                // The underline is drawn via the cell's `und(unln::line)`
                // attribute (with `unc(col_uline)` for color), instead of the
                // legacy `_` glyph -- this matches the tile.hpp command-search
                // bar style and keeps the strip continuous beneath text glyphs.
                // When the query is non-empty the rightmost 3 cells are reserved
                // for the clear-query button, so text rendering is capped to the
                // remaining (input_w - btn_width) cells.  At very narrow widths
                // (input_w < min_input_w + btn_width) the clear button cannot
                // fit so it stays hidden even with a non-empty query.
                auto clear_visible = !st.query.empty() && L.clear_fits;
                auto eff_input_w = clear_visible ? (L.input_w - btn_width) : L.input_w;
                auto input_bg = st.hover == hov_input ? col_hov_bg : col_bg;
                canvas.fill(rect{{ x0 + L.input_x0, mid_y }, { L.input_w, 1 }},
                    [input_bg](cell& c){ c.bgc(input_bg).fgc(col_text).txt(ch_sp).cur(text_cursor::none).und(unln::line).unc(col_uline); });

                // Bound scroll window so the caret stays visible.
                auto total = cp_len(st.query);
                if (st.caret_cp < 0)          st.caret_cp = 0;
                if (st.caret_cp > total)      st.caret_cp = total;
                if (st.scroll_cp < 0)         st.scroll_cp = 0;
                if (st.scroll_cp > st.caret_cp) st.scroll_cp = st.caret_cp;
                if (st.caret_cp - st.scroll_cp >= eff_input_w)
                {
                    st.scroll_cp = st.caret_cp - eff_input_w + 1;
                }
                // After inserting at end, keep caret at right edge but leave
                // 1-cell space for the caret indicator.
                if (st.caret_cp == total && total - st.scroll_cp >= eff_input_w)
                {
                    st.scroll_cp = total - eff_input_w + 1;
                    if (st.scroll_cp < 0) st.scroll_cp = 0;
                }

                auto visible_text = slice_cp(st.query, st.scroll_cp, eff_input_w);
                // Overlay text glyphs on top of the underlined strip. Each
                // overlaid cell preserves the underline attribute so the
                // strip stays continuous beneath the text.
                auto i_col = x0 + L.input_x0;
                auto it   = visible_text.begin();
                auto stop = visible_text.end();
                while (it != stop && i_col <= x0 + L.input_x0 + eff_input_w - 1)
                {
                    auto begin = it;
                    ++it;
                    while (it != stop && ((byte)*it & 0xC0) == 0x80) ++it;
                    auto glyph = text(begin, it);
                    canvas.fill(rect{{ i_col, mid_y }, { 1, 1 }},
                        [&glyph, input_bg](cell& c){ c.bgc(input_bg).fgc(col_text).txt(glyph).cur(text_cursor::none).und(unln::line).unc(col_uline); });
                    ++i_col;
                }

                // Caret indicator (I-bar style, drawn as an inverted cell).
                auto caret_col = x0 + L.input_x0 + (st.caret_cp - st.scroll_cp);
                if (caret_col >= x0 + L.input_x0 && caret_col <= x0 + L.input_x0 + eff_input_w - 1)
                {
                    canvas.fill(rect{{ caret_col, mid_y }, { 1, 1 }},
                        [](cell& c)
                        {
                            // Draw a visible block cursor (own cursor). The
                            // terminal cursor is already suppressed everywhere
                            // inside the bar by cur(text_cursor::none) above.
                            // Drop the underline attribute on the caret cell
                            // so the I-bar block reads as a solid highlight.
                            c.bgc(col_caret).fgc(col_bg).cur(text_cursor::none).und(unln::none).unc(0);
                            if (c.txt().empty()) c.txt(ch_sp);
                        });
                }

                // Match counter: right-aligned in counter_text_width (9) cells,
                // rendered at count_text_x0 in col_count.
                // Padding cells (count_x0 and count_x1) inherit the bar background.
                // Skipped entirely when the bar is too narrow to host the counter.
                if (L.show_counter)
                {
                    auto txt = format_counter(st.index, st.total);
                    auto i = 0;
                    for (auto ch : txt)
                    {
                        auto cx = x0 + L.count_text_x0 + i;
                        if (cx < x0 + 1 || cx >= x0 + w - 1) { ++i; continue; }
                        auto glyph = text{ ch };
                        canvas.fill(rect{{ cx, mid_y }, { 1, 1 }},
                            [&glyph](cell& c){ c.bgc(col_bg).fgc(col_count).txt(glyph).cur(text_cursor::none); });
                        ++i;
                    }
                }

                // Buttons: [ ↑ ] [ ↓ ] [ × ] -- each 3 cells wide, glyph in
                // the center cell, flanking cells are spaces (so the hover
                // highlight covers the whole 3-cell hitbox).
                // The direction (↑/↓) buttons are rendered as a radio pair:
                // the one matching st.dir is drawn with col_act_bg so the
                // user instantly sees which way Enter will jump.
                // `underlined` = true draws the underline attribute under the
                // whole 3-cell button so it visually connects to the input
                // strip (used for the in-input clear-query button only).
                auto draw_btn = [&](si32 center_x, view glyph, argb fg, bool hov, bool active, bool underlined = faux)
                {
                    auto bg = active ? col_act_bg
                                     : (hov ? col_hov_bg : col_bg);
                    auto g  = text{ glyph };
                    auto sp = text{ ch_sp };
                    auto lx = center_x - 1;  // leftmost cell of 3-cell button
                    for (auto k = si32{ 0 }; k < btn_width; ++k)
                    {
                        auto cx = lx + k;
                        if (cx < 1 || cx >= w - 1) continue;
                        auto& ch = (k == 1) ? g : sp;
                        canvas.fill(rect{{ x0 + cx, mid_y }, { 1, 1 }},
                            [&ch, fg, bg, underlined](cell& c)
                            {
                                c.bgc(bg).fgc(fg).txt(ch).cur(text_cursor::none);
                                if (underlined) c.und(unln::line).unc(col_uline);
                            });
                    }
                };
                if (L.show_dir)
                {
                    draw_btn(L.btn_up_x, "↑", col_btn,   st.hover == hov_btn_up, st.dir == dir_up);
                    draw_btn(L.btn_dn_x, "↓", col_btn,   st.hover == hov_btn_dn, st.dir == dir_down);
                }
                draw_btn(L.btn_x_x,  "×", col_btn_x, st.hover == hov_btn_x,  faux);
                if (clear_visible)
                {
                    // Underlined so the clear button reads as a continuation
                    // of the input strip rather than a free-standing control.
                    draw_btn(L.btn_clear_x, "×", col_btn_x, st.hover == hov_btn_clear, faux, true);
                }
            };

            // Mouse click handling on the bar: detect which button was hit.
            bar->active();
            auto bar_self = std::weak_ptr<ui::base>(bar);
            bar->invoke([state_ptr, layout_of, render_bar, bar_self](auto& boss)
            {
                // Custom render.
                boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (state_ptr, render_bar))
                {
                    auto full = parent_canvas.full();
                    render_bar(parent_canvas, full, *state_ptr);
                };
                // Left-click: dispatch to the appropriate action.
                boss.on(tier::mouserelease, input::key::LeftClick,
                    [state_ptr, layout_of, bar_self](hids& gear)
                    {
                        auto& st = *state_ptr;
                        // gear.coord is widget-local.
                        auto mx = si32(gear.coord.x);
                        auto my = si32(gear.coord.y);
                        if (my != 1) { gear.dismiss(true); return; } // only middle row is interactive
                        // Use the actual current bar width (responsive
                        // layout): when the host terminal is narrow some
                        // controls are dropped from the bar, so hit-testing
                        // must be aware of which features are present.
                        auto bar_ptr = bar_self.lock();
                        if (!bar_ptr) { gear.dismiss(true); return; }
                        auto outer_w = bar_ptr->base::size().x;
                        auto L = layout_of(outer_w);
                        if (!L.valid) { gear.dismiss(true); return; }
                        auto clear_visible = !st.query.empty() && L.clear_fits;
                        // Each button is 3 cells wide; hit-test covers the
                        // whole span (center ± 1).
                        if (mx >= L.btn_x_x - 1 && mx <= L.btn_x_x + 1)
                        {
                            gear.owner.base::signal(tier::anycast, ui::terminal::events::find::toggle, 0);
                        }
                        else if (L.show_dir && mx >= L.btn_up_x - 1 && mx <= L.btn_up_x + 1)
                        {
                            // Switch direction to "up" and re-seed the match
                            // list from the bottom (find::result will echo
                            // the updated counter).
                            st.dir = dir_up;
                            // Force immediate repaint so the active-direction
                            // highlight updates right away.  On Windows the
                            // backend may return an unchanged 0/0 result when
                            // the query is empty, which would otherwise leave
                            // the bar visually stale until the next mouse-move
                            // or mouse-leave event.
                            if (auto b = bar_self.lock()) b->base::deface();
                            auto req = ui::terminal::events::find_req{ st.query, dir_up };
                            gear.owner.base::signal(tier::anycast, ui::terminal::events::find::request, req);
                        }
                        else if (L.show_dir && mx >= L.btn_dn_x - 1 && mx <= L.btn_dn_x + 1)
                        {
                            // Switch direction to "down" and re-seed the match
                            // list from the top.
                            st.dir = dir_down;
                            // Same rationale as the dir_up case above.
                            if (auto b = bar_self.lock()) b->base::deface();
                            auto req = ui::terminal::events::find_req{ st.query, dir_down };
                            gear.owner.base::signal(tier::anycast, ui::terminal::events::find::request, req);
                        }
                        else if (clear_visible && mx >= L.btn_clear_x - 1 && mx <= L.btn_clear_x + 1)
                        {
                            // Clear-query button: wipe the query, reset caret/scroll,
                            // notify the backend (empty query clears highlights).
                            st.query.clear();
                            st.caret_cp  = 0;
                            st.scroll_cp = 0;
                            if (auto b = bar_self.lock()) b->base::deface();
                            auto req = ui::terminal::events::find_req{ st.query, st.dir };
                            gear.owner.base::signal(tier::anycast, ui::terminal::events::find::request, req);
                        }
                        else if (mx >= L.input_x0 && mx <= L.input_x1)
                        {
                            // Click positions the caret.  When the clear
                            // button overlaps the rightmost 3 input cells
                            // it is handled above; here we cap the caret
                            // position to the text region.
                            auto eff_input_w = clear_visible ? (L.input_w - btn_width) : L.input_w;
                            auto total = si32{ 0 };
                            for (auto c : st.query) total += ((byte)c & 0xC0) != 0x80;
                            auto rel = mx - L.input_x0;
                            if (rel >= eff_input_w) rel = eff_input_w - 1;
                            auto new_cp = st.scroll_cp + rel;
                            if (new_cp > total) new_cp = total;
                            st.caret_cp = new_cp;
                            gear.owner.base::signal(tier::anycast, ui::terminal::events::find::status, 1); // trigger reflow
                        }
                        gear.dismiss(true);
                    });
                // Mouse-move: update hover zone and repaint if it changed.
                boss.on(tier::mouserelease, input::key::MouseMove,
                    [state_ptr, layout_of, bar_self](hids& gear)
                    {
                        auto& st = *state_ptr;
                        auto mx = si32(gear.coord.x);
                        auto my = si32(gear.coord.y);
                        auto bar_ptr = bar_self.lock();
                        if (!bar_ptr) return;
                        auto outer_w = bar_ptr->base::size().x;
                        auto L = layout_of(outer_w);
                        if (!L.valid) return;
                        auto clear_visible = !st.query.empty() && L.clear_fits;
                        auto new_hover = hov_none;
                        if (my == 1)
                        {
                            // 3-cell hitbox per button (center ± 1).
                            if      (mx >= L.btn_x_x  - 1 && mx <= L.btn_x_x  + 1) new_hover = hov_btn_x;
                            else if (L.show_dir && mx >= L.btn_dn_x - 1 && mx <= L.btn_dn_x + 1) new_hover = hov_btn_dn;
                            else if (L.show_dir && mx >= L.btn_up_x - 1 && mx <= L.btn_up_x + 1) new_hover = hov_btn_up;
                            else if (clear_visible
                                  && mx >= L.btn_clear_x - 1
                                  && mx <= L.btn_clear_x + 1)                       new_hover = hov_btn_clear;
                            else if (mx >= L.input_x0     && mx <= L.input_x1)     new_hover = hov_input;
                        }
                        if (new_hover != st.hover)
                        {
                            st.hover = new_hover;
                            if (auto b = bar_self.lock()) b->base::deface();
                        }
                    });
                // Mouse-leave: clear hover zone.
                boss.on(tier::mouserelease, input::key::MouseLeave,
                    [state_ptr, bar_self](hids&)
                    {
                        auto& st = *state_ptr;
                        if (st.hover != hov_none)
                        {
                            st.hover = hov_none;
                            if (auto b = bar_self.lock()) b->base::deface();
                        }
                    });
                // Backend -> UI counter update: refresh total/index and repaint.
                boss.LISTEN(tier::anycast, ui::terminal::events::find::result, res, -, (state_ptr))
                {
                    auto& st = *state_ptr;
                    auto old_total = st.total;
                    auto old_index = st.index;
                    st.total = res.total;
                    st.index = res.index;
                    if (st.total != old_total || st.index != old_index)
                    {
                        boss.base::deface();
                    }
                };
            });

            // Overlay-level toggle + keyboard hook logic.
            auto term_weak = std::weak_ptr<ui::base>(term);
            auto overlay_weak = std::weak_ptr<ui::base>(overlay);
            auto bar_weak = std::weak_ptr<ui::base>(bar);
            overlay->invoke([state_ptr, kbd_hook, pending_unhook, term_weak, overlay_weak, bar_weak, cp_len, byte_of_cp](auto& boss)
            {
                boss.LISTEN(tier::anycast, ui::terminal::events::find::toggle, state, -,
                            (state_ptr, kbd_hook, pending_unhook, term_weak, overlay_weak, bar_weak, cp_len, byte_of_cp))
                {
                    auto cur = !boss.base::hidden;
                    auto tgt = state < 0 ? !cur : !!state;
                    if (cur == tgt) return;
                    boss.base::hidden = !tgt;
                    state_ptr->visible = tgt;
                    boss.base::reflow();
                    boss.base::signal(tier::anycast, ui::terminal::events::find::status, (si32)tgt);
                    if (tgt) // show
                    {
                        // Starting state: empty counter, Down is active.
                        state_ptr->total = 0;
                        state_ptr->index = 0;
                        *pending_unhook = faux;
                        if (!state_ptr->query.empty()) // Re-run search if there is a pending query.
                        {
                            auto req = ui::terminal::events::find_req{ state_ptr->query, state_ptr->dir };
                            boss.base::signal(tier::anycast, ui::terminal::events::find::request, req);
                        }
                        if (auto t = term_weak.lock(); t && !*kbd_hook)
                        {
                            t->bell::submit(tier::preview, input::events::keybd::any, *kbd_hook)
                                = [state_ptr, kbd_hook, pending_unhook, bar_weak, cp_len, byte_of_cp](hids& gear) mutable
                            {
                                if (gear.keybd::handled) return;
                                if (gear.payload == input::keybd::type::keypress
                                 && gear.keystat == input::key::interrupted) return;
                                if (gear.payload != input::keybd::type::keypress
                                 && gear.payload != input::keybd::type::keypaste) return;
                                if (*pending_unhook) // trailing release after close
                                {
                                    if (gear.payload == input::keybd::type::keypress)
                                    {
                                        gear.set_handled(faux);
                                        if (gear.keystat == input::key::released)
                                        {
                                            *pending_unhook = faux;
                                            kbd_hook->reset();
                                        }
                                    }
                                    return;
                                }
                                if (!state_ptr->visible) return;
                                if (gear.payload == input::keybd::type::keypress
                                 && gear.keystat == input::key::released)
                                {
                                    gear.set_handled(faux);
                                    return;
                                }
                                // keypaste: text pasted from external terminal emulator.
                                // gear.cluster holds the full paste payload.
                                // Insert it into the search query and consume the event
                                // so it does not leak into the terminal.
                                if (gear.payload == input::keybd::type::keypaste)
                                {
                                    auto& st = *state_ptr;
                                    auto changed = faux;
                                    auto query_changed = faux;
                                    auto src = qiew{ gear.cluster };
                                    auto buf = text{};
                                    buf.reserve(src.size());
                                    for (size_t i = 0; i < src.size();)
                                    {
                                        auto c = (byte)src[i];
                                        if (c < 0x20 || c == 0x7f) { ++i; continue; }
                                        auto n = size_t{ 1 };
                                        if      ((c & 0xE0) == 0xC0) n = 2;
                                        else if ((c & 0xF0) == 0xE0) n = 3;
                                        else if ((c & 0xF8) == 0xF0) n = 4;
                                        if (i + n > src.size()) n = src.size() - i;
                                        buf.append(src.data() + i, n);
                                        i += n;
                                    }
                                    if (buf.size())
                                    {
                                        auto pos = byte_of_cp(st.query, st.caret_cp);
                                        st.query.insert(pos, buf);
                                        st.caret_cp += cp_len(buf);
                                        changed = true;
                                        query_changed = true;
                                    }
                                    if (changed) { if (auto b = bar_weak.lock()) b->base::deface(); }
                                    if (query_changed)
                                    {
                                        auto req = ui::terminal::events::find_req{ st.query, st.dir };
                                        gear.owner.base::signal(tier::anycast, ui::terminal::events::find::request, req);
                                    }
                                    gear.set_handled(faux);
                                    return;
                                }
                                auto& st = *state_ptr;
                                auto changed = faux;        // visual change (caret or query) -> repaint
                                auto query_changed = faux;  // query text actually changed -> re-seed search
                                auto fire = si32{ 0 }; // -1 rev, +1 fwd
                                auto close = faux;
                                auto k = gear.keybd::generic();
                                auto total = cp_len(st.query);
                                auto ctrl  = !!(gear.ctlstat & hids::anyCtrl);
                                auto alt   = !!(gear.ctlstat & hids::anyAlt);
                                auto shift = !!(gear.ctlstat & hids::anyShift);
                                // Emacs-style word boundary helpers.  A word is
                                // a run of non-whitespace codepoints; we scan
                                // by codepoint so UTF-8 clusters stay intact.
                                auto is_ws_cp = [&](si32 cp) -> bool
                                {
                                    if (cp < 0 || cp >= total) return true;
                                    auto i = byte_of_cp(st.query, cp);
                                    auto c = (byte)st.query[i];
                                    return c == 0x20 || c == 0x09;
                                };
                                auto prev_word_cp = [&](si32 cp) -> si32
                                {
                                    if (cp <= 0) return 0;
                                    // Skip trailing whitespace, then skip word chars.
                                    while (cp > 0 && is_ws_cp(cp - 1)) --cp;
                                    while (cp > 0 && !is_ws_cp(cp - 1)) --cp;
                                    return cp;
                                };
                                auto next_word_cp = [&](si32 cp) -> si32
                                {
                                    if (cp >= total) return total;
                                    // Skip leading whitespace, then skip word chars.
                                    while (cp < total && is_ws_cp(cp)) ++cp;
                                    while (cp < total && !is_ws_cp(cp)) ++cp;
                                    return cp;
                                };
                                auto erase_cp_range = [&](si32 from_cp, si32 to_cp)
                                {
                                    if (from_cp < 0)     from_cp = 0;
                                    if (to_cp   > total) to_cp   = total;
                                    if (from_cp >= to_cp) return;
                                    auto a = byte_of_cp(st.query, from_cp);
                                    auto b = byte_of_cp(st.query, to_cp);
                                    st.query.erase(a, b - a);
                                    if (st.caret_cp > to_cp)        st.caret_cp -= (to_cp - from_cp);
                                    else if (st.caret_cp > from_cp) st.caret_cp  = from_cp;
                                    changed = true;
                                    query_changed = true;
                                };
                                if (k == input::key::Esc)
                                {
                                    close = true;
                                }
                                else if (k == input::key::Backspace)
                                {
                                    if (alt) // Alt+Backspace: kill word backward.
                                    {
                                        erase_cp_range(prev_word_cp(st.caret_cp), st.caret_cp);
                                    }
                                    else if (st.caret_cp > 0)
                                    {
                                        auto a = byte_of_cp(st.query, st.caret_cp - 1);
                                        auto b = byte_of_cp(st.query, st.caret_cp);
                                        st.query.erase(a, b - a);
                                        --st.caret_cp;
                                        changed = true;
                                        query_changed = true;
                                    }
                                }
                                else if (k == input::key::KeyDelete || k == input::key::NumpadDelete)
                                {
                                    if (alt) // Alt+Delete: kill word forward.
                                    {
                                        erase_cp_range(st.caret_cp, next_word_cp(st.caret_cp));
                                    }
                                    else if (st.caret_cp < total)
                                    {
                                        auto a = byte_of_cp(st.query, st.caret_cp);
                                        auto b = byte_of_cp(st.query, st.caret_cp + 1);
                                        st.query.erase(a, b - a);
                                        changed = true;
                                        query_changed = true;
                                    }
                                }
                                // Emacs cursor-movement bindings (no modifiers
                                // beyond Ctrl/Alt; Shift is ignored here so
                                // Ctrl+Shift+A still goes to start-of-line).
                                else if (ctrl && !alt && k == input::key::KeyA)
                                {
                                    if (st.caret_cp != 0) { st.caret_cp = 0; changed = true; }
                                }
                                else if (ctrl && !alt && k == input::key::KeyE)
                                {
                                    if (st.caret_cp != total) { st.caret_cp = total; changed = true; }
                                }
                                else if (ctrl && !alt && k == input::key::KeyB)
                                {
                                    if (st.caret_cp > 0) { --st.caret_cp; changed = true; }
                                }
                                else if (ctrl && !alt && k == input::key::KeyF)
                                {
                                    if (st.caret_cp < total) { ++st.caret_cp; changed = true; }
                                }
                                else if (ctrl && !alt && k == input::key::KeyD)
                                {
                                    // Ctrl+D: delete char forward.
                                    if (st.caret_cp < total)
                                    {
                                        auto a = byte_of_cp(st.query, st.caret_cp);
                                        auto b = byte_of_cp(st.query, st.caret_cp + 1);
                                        st.query.erase(a, b - a);
                                        changed = true;
                                        query_changed = true;
                                    }
                                }
                                else if (ctrl && !alt && k == input::key::KeyH)
                                {
                                    // Ctrl+H: delete char backward (Backspace).
                                    if (st.caret_cp > 0)
                                    {
                                        auto a = byte_of_cp(st.query, st.caret_cp - 1);
                                        auto b = byte_of_cp(st.query, st.caret_cp);
                                        st.query.erase(a, b - a);
                                        --st.caret_cp;
                                        changed = true;
                                        query_changed = true;
                                    }
                                }
                                else if (ctrl && !alt && k == input::key::KeyK)
                                {
                                    // Ctrl+K: kill to end of line.
                                    erase_cp_range(st.caret_cp, total);
                                }
                                else if (ctrl && !alt && k == input::key::KeyU)
                                {
                                    // Ctrl+U: kill to beginning of line.
                                    erase_cp_range(0, st.caret_cp);
                                }
                                else if (ctrl && !alt && k == input::key::KeyW)
                                {
                                    // Ctrl+W: kill word backward.
                                    erase_cp_range(prev_word_cp(st.caret_cp), st.caret_cp);
                                }
                                else if (alt && !ctrl && k == input::key::KeyB)
                                {
                                    // Alt+B: back one word.
                                    auto np = prev_word_cp(st.caret_cp);
                                    if (np != st.caret_cp) { st.caret_cp = np; changed = true; }
                                }
                                else if (alt && !ctrl && k == input::key::KeyF)
                                {
                                    // Alt+F: forward one word.
                                    auto np = next_word_cp(st.caret_cp);
                                    if (np != st.caret_cp) { st.caret_cp = np; changed = true; }
                                }
                                else if (alt && !ctrl && k == input::key::KeyD)
                                {
                                    // Alt+D: kill word forward.
                                    erase_cp_range(st.caret_cp, next_word_cp(st.caret_cp));
                                }
                                else if (k == input::key::KeyLeftArrow || k == input::key::NumpadLeftArrow)
                                {
                                    if (st.caret_cp > 0) { --st.caret_cp; changed = true; }
                                }
                                else if (k == input::key::KeyRightArrow || k == input::key::NumpadRightArrow)
                                {
                                    if (st.caret_cp < total) { ++st.caret_cp; changed = true; }
                                }
                                else if (k == input::key::KeyHome || k == input::key::NumpadHome)
                                {
                                    if (st.caret_cp != 0) { st.caret_cp = 0; changed = true; }
                                }
                                else if (k == input::key::KeyEnd || k == input::key::NumpadEnd)
                                {
                                    if (st.caret_cp != total) { st.caret_cp = total; changed = true; }
                                }
                                else if (k == input::key::KeyEnter || k == input::key::NumpadEnter)
                                {
                                    // Enter navigates in the active direction;
                                    // Shift+Enter navigates the opposite way.
                                    // Note: under pty/nested input protocols the
                                    // Shift modifier on Enter is often lost; use
                                    // Up/Down arrows below for a portable way to
                                    // navigate in a fixed direction.
                                    auto reverse = shift;
                                    fire = reverse ? -st.dir : st.dir;
                                }
                                else if (k == input::key::KeyUpArrow || k == input::key::NumpadUpArrow)
                                {
                                    // Up arrow = navigate toward scrollback start,
                                    // regardless of the active arrow direction.
                                    fire = dir_up;
                                }
                                else if (k == input::key::KeyDownArrow || k == input::key::NumpadDownArrow)
                                {
                                    // Down arrow = navigate toward scrollback end,
                                    // regardless of the active arrow direction.
                                    fire = dir_down;
                                }
                                else if (ctrl && !alt && k == input::key::KeyP)
                                {
                                    // Ctrl+P: previous match (up).
                                    fire = dir_up;
                                }
                                else if (ctrl && !alt && k == input::key::KeyN)
                                {
                                    // Ctrl+N: next match (down).
                                    fire = dir_down;
                                }
                                else if (!ctrl && !alt && !shift && k == input::key::Tab)
                                {
                                    // Tab: toggle the active navigation direction (↑ ↔ ↓).
                                    // Re-seed the search from the new direction's start
                                    // edge so the match counter and viewport highlight
                                    // update immediately.  The direction button highlight
                                    // also repaints right away via the changed flag below.
                                    st.dir = (st.dir == dir_up) ? dir_down : dir_up;
                                    changed = true;
                                    auto req = ui::terminal::events::find_req{ st.query, st.dir };
                                    gear.owner.base::signal(tier::anycast, ui::terminal::events::find::request, req);
                                }
                                else if (ctrl && !alt && k == input::key::KeyY)
                                {
                                    // Ctrl+Y: yank (paste) from clipboard at caret.
                                    // Request clipboard data into gear.board::cargo,
                                    // then insert its plain-text payload, filtering
                                    // control bytes to stay within the search query.
                                    gear.owner.base::signal(tier::request, input::events::clipboard, gear);
                                    auto& data = gear.board::cargo;
                                    if (data.utf8.size())
                                    {
                                        auto src = qiew{ data.utf8 };
                                        auto buf = text{};
                                        buf.reserve(src.size());
                                        // Walk UTF-8; drop C0/DEL, keep everything else
                                        // (including multi-byte clusters).  Newlines
                                        // and tabs are stripped to keep the query on
                                        // a single line.
                                        for (size_t i = 0; i < src.size();)
                                        {
                                            auto c = (byte)src[i];
                                            if (c < 0x20 || c == 0x7f)
                                            {
                                                ++i;
                                                continue;
                                            }
                                            // Determine UTF-8 cluster length.
                                            auto n = size_t{ 1 };
                                            if      ((c & 0xE0) == 0xC0) n = 2;
                                            else if ((c & 0xF0) == 0xE0) n = 3;
                                            else if ((c & 0xF8) == 0xF0) n = 4;
                                            if (i + n > src.size()) n = src.size() - i;
                                            buf.append(src.data() + i, n);
                                            i += n;
                                        }
                                        if (buf.size())
                                        {
                                            auto pos = byte_of_cp(st.query, st.caret_cp);
                                            st.query.insert(pos, buf);
                                            st.caret_cp += cp_len(buf);
                                            changed = true;
                                            query_changed = true;
                                        }
                                    }
                                }
                                else if (!ctrl && !alt && gear.cluster.size())
                                {
                                    // Accept any non-control cluster (UTF-8 aware).
                                    // Ctrl/Alt combos are handled above; don't let
                                    // their generated control bytes leak in here.
                                    auto first = (byte)gear.cluster.front();
                                    if (first >= 0x20 && first != 0x7f)
                                    {
                                        auto pos = byte_of_cp(st.query, st.caret_cp);
                                        st.query.insert(pos, gear.cluster);
                                        st.caret_cp += cp_len(gear.cluster);
                                        changed = true;
                                        query_changed = true;
                                    }
                                }
                                if (changed || fire)
                                {
                                    if (auto b = bar_weak.lock()) b->base::deface();
                                }
                                // Any query edit re-seeds the match from the
                                // current direction's start edge, so the user
                                // immediately sees the first (or last) match
                                // and the counter reflects the new total.
                                if (query_changed && !fire)
                                {
                                    auto req = ui::terminal::events::find_req{ st.query, st.dir };
                                    gear.owner.base::signal(tier::anycast, ui::terminal::events::find::request, req);
                                }
                                if (fire)
                                {
                                    auto req = ui::terminal::events::find_req{ st.query, fire };
                                    gear.owner.base::signal(tier::anycast, ui::terminal::events::find::request, req);
                                }
                                if (close)
                                {
                                    *pending_unhook = true;
                                    gear.owner.base::signal(tier::anycast, ui::terminal::events::find::toggle, 0);
                                }
                                gear.set_handled(faux); // swallow absolutely everything
                            };
                        }
                    }
                    else // hide
                    {
                        if (!*pending_unhook) kbd_hook->reset();
                    }
                };
            });
        }

        auto sb = layers->attach(ui::fork::ctor());
        auto vt = sb->attach(slot::_2, ui::grip<axis::Y>::ctor(scroll));
        auto& term_bgc = term->get_color().bgc();
        auto& drawfx = term->base::field([&](auto& boss, auto& canvas, auto scrollbar_grip, auto master_len, auto master_box, auto master_pos, auto wide)
        {
            static auto box1 = "▄"sv;
            static auto box2 = ' ';
            if (ui::drawfx::visible(master_len, master_box, master_pos))
            {
                if (wide) // Draw full scrollbar on mouse hover.
                {
                    canvas.fill([&](cell& c){ c.txt(box2).link(boss.bell::id).xlight().bgc().mix(window_clr.bgc()); });
                    canvas.fill(scrollbar_grip, [&](cell& c){ c.bgc().xlight(2); });
                }
                else
                {
                    canvas.fill([&](cell& c){ c.txt(box1).fgc(c.bgc()).bgc(term_bgc).fgc().mix(window_clr.bgc()); });
                    canvas.fill(scrollbar_grip, [&](cell& c){ c.link(boss.bell::id).fgc().xlight(2); });
                }
            }
            else canvas.fill([&](cell& c){ c.txt(box1).fgc(c.bgc()).bgc(term_bgc).fgc().mix(window_clr.bgc()); });
        });
        auto hz = term_stat_area->attach(slot::_2, ui::grip<axis::X>::ctor(scroll, drawfx))
            ->limits({ -1, 1 }, { -1, 1 });

        auto [slot1, cover, menu_data] = app::shared::menu::load(config);
        auto menu = object->attach(slot::_1, slot1)
            ->shader(window_clr);

        cover->invoke([&, &slot1 = slot1](auto& boss) //todo clang 15.0.0 still disallows capturing structured bindings (wait for clang 16.0.0)
        {
            auto& bar = boss.base::field(cell{ "▀"sv }.link(slot1->id));
            auto& winsz = boss.base::field(dot_00);
            auto& visible = boss.base::field(slot1->back() != boss.This());
            auto& check_state = boss.base::field([state = true, &winsz, &visible](base& boss) mutable
            {
                if (std::exchange(state, visible || winsz.y != 1) != state)
                {
                    boss.base::riseup(tier::preview, e2::form::prop::ui::cache, state);
                }
            });
            boss.LISTEN(tier::release, e2::form::state::visible, menu_visible)
            {
                visible = menu_visible;
                check_state(boss);
            };
            boss.LISTEN(tier::anycast, e2::form::upon::resized, new_area)
            {
                winsz = new_area.size;
                check_state(boss);
            };
            boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (borders))
            {
                auto full = parent_canvas.full();
                if (winsz.y != 1 && borders)
                {
                    parent_canvas.cage(full, borders, [&](cell& c){ c.txt(whitespace).link(bar); });
                    full -= borders;
                }
                auto bgc = winsz.y != 1 ? term_bgc : 0;
                parent_canvas.fill(full, [&](cell& c){ c.fgc(c.bgc()).bgc(bgc).txt(bar).link(bar); });
            };
        });
        term->invoke([&, confirm_close, window_shadow = std::weak_ptr<ui::base>(window)](auto& boss)
        {
            ui_term_events(boss, appcfg, confirm_close, window_shadow);
        });
        return window;
    };

    app::shared::initialize teletype_builder{ app::teletype::id, build_teletype };
    app::shared::initialize terminal_builder{ app::terminal::id, build_terminal };
}
