// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#pragma once

#if defined(__clang__) || defined(__APPLE__)
    #pragma clang diagnostic ignored "-Wunused-variable"
    #pragma clang diagnostic ignored "-Wunused-function"
#endif

#include "console.hpp"
#include "system.hpp"
#include "terminal.hpp"

namespace netxs::app
{
    namespace fs = std::filesystem;
    using namespace std::placeholders;
    using namespace netxs::ui;
}

namespace netxs::app::shared
{
    static const auto version = "v2026.01.16";
    static const auto repository = "https://github.com/directvt/vtm";
    static const auto usr_config = "~/.config/vtm/settings.xml"s;
    static const auto sys_config = "/etc/vtm/settings.xml"s;

    const auto closing_on_quit = [](auto& boss)
    {
        boss.LISTEN(tier::anycast, e2::form::proceed::quit::any, fast)
        {
            boss.base::riseup(tier::release, e2::form::proceed::quit::one, fast);
        };
    };
    const auto closing_by_gesture = [](auto& boss)
    {
        boss.on(tier::mouserelease, input::key::LeftRightClick, [&](hids& gear)
        {
            auto backup = boss.This();
            boss.base::riseup(tier::release, e2::form::proceed::quit::one, true);
            gear.dismiss();
        });
        boss.on(tier::mouserelease, input::key::MiddleClick, [&](hids& gear)
        {
            auto backup = boss.This();
            boss.base::riseup(tier::release, e2::form::proceed::quit::one, true);
            gear.dismiss();
        });
    };
    // confirm_dialog_text: Text content for a confirmation dialog.  Pass one of
    // the scenario presets below (confirm_text_*) to show_close_confirmation, or
    // construct a custom instance.  Long messages wrap; button labels are
    // centered within a fixed 18-column button cell.
    struct confirm_dialog_text
    {
        text message       = "Confirm closing this window?";
        text confirm_label = "Confirm";
        text cancel_label  = "Cancel";
    };
    // Scenario presets.  Use these at call sites so wording stays consistent.
    inline const auto confirm_text_window_close      = confirm_dialog_text{ "Confirm closing this window?",     "Confirm",    "Cancel" };
    inline const auto confirm_text_close_pane        = confirm_dialog_text{ "Close the focused pane?",          "Close",      "Cancel" };
    inline const auto confirm_text_close_slot        = confirm_dialog_text{ "Close the focused pane and remove this slot?", "Close",      "Cancel" };
    inline const auto confirm_text_shutdown          = confirm_dialog_text{ "Shut down all panes and exit?",    "Shutdown",   "Cancel" };
    inline const auto confirm_text_destroy_workspace = confirm_dialog_text{ "Destroy the current workspace?",   "Destroy",    "Cancel" };
    inline const auto confirm_text_rerun_application = confirm_dialog_text{ "Restart the focused application?", "Restart",    "Cancel" };
    inline const auto confirm_text_disconnect        = confirm_dialog_text{ "Disconnect from this session?",    "Disconnect", "Cancel" };
    // show_close_confirmation: Display a centered MessageBox dialog overlay
    // asking the user to confirm a destructive action. Attach the overlay to the
    // specified cake parent. on_confirm is called when the user accepts.
    // texts selects the message and button labels; defaults to window_close.
    static void show_close_confirmation(ui::base& parent,
                                        std::function<void()> on_confirm,
                                        std::function<void()> on_cancel = {},
                                        confirm_dialog_text const& texts = confirm_text_window_close)
    {
        // Dialog card layout — referenced by both the layout setup below and
        // the hit-test in the overlay MouseMove handler. Keep these in sync.
        //
        //   ████████████████████████████████████████████  ┐ pad_t = 1
        //   ███Confirm closing this window?█████████████ ─┤ message (msg_h rows, wraps if long)
        //   ████████████████████████████████████████████ ─┤ gap_h = 1 (always)
        //   ███     Confirm      ██      Cancel      ███ ─┤ button row (btn_row_h = 1)
        //   ████████████████████████████████████████████  ┘ pad_b = 1
        //    └ pad_l = 3                    pad_r = 3 ┘
        //
        static constexpr auto dialog_w   = si32{ 44 };
        static constexpr auto pad_l      = si32{ 3 };
        static constexpr auto pad_r      = si32{ 3 };
        static constexpr auto pad_t      = si32{ 1 };
        static constexpr auto pad_b      = si32{ 1 };
        static constexpr auto gap_h      = si32{ 1 }; // Always-on vertical gap between message and buttons.
        static constexpr auto btn_grip   = si32{ 2 };
        static constexpr auto btn_row_h  = si32{ 1 };
        static constexpr auto btn_w      = (dialog_w - pad_l - pad_r - btn_grip) / 2; // 18
        static constexpr auto inner_w    = dialog_w - pad_l - pad_r;                  // 38
        // X-ranges of each button within the dialog card: [x0, x1).
        static constexpr auto confirm_x0 = pad_l;                                     //  3
        static constexpr auto confirm_x1 = confirm_x0 + btn_w;                        // 21
        static constexpr auto cancel_x0  = confirm_x1 + btn_grip;                     // 23
        static constexpr auto cancel_x1  = cancel_x0 + btn_w;                         // 41

        // Runtime: dialog height grows with the wrapped message so the gap
        // between message and buttons stays exactly gap_h regardless of
        // how many rows the message wraps to. utf::length-over-inner_w is
        // an upper bound on the row count for char-wrap and is accurate
        // for the predefined messages in this header.
        auto msg_chars = (si32)utf::length(texts.message);
        auto msg_h     = std::max(si32{ 1 }, (msg_chars + inner_w - 1) / inner_w);
        auto dialog_h  = pad_t + msg_h + gap_h + btn_row_h + pad_b;
        // Button row y-offset within the dialog card.
        auto btn_row_y = pad_t + msg_h + gap_h;

        // Guard: don't show multiple dialogs at once.
        auto& dialog_active = parent.base::property("msgbox.active", faux);
        if (dialog_active) return;
        dialog_active = true;

        // Create full-screen overlay (attaches to parent cake as topmost layer).
        auto overlay_ptr = ui::cake::ctor()
            ->alignment({ snap::both, snap::both });

        auto overlay_shadow = ptr::shadow(overlay_ptr);
        auto parent_shadow = ptr::shadow(parent.This());

        // Shared hook controlling the keyboard interceptor subscription on the
        // parent.  Resetting this hook unsubscribes the handler.
        auto kbd_hook = ptr::shared<hook>();
        // Shared flag: once set, the dialog visuals are dismissed but the
        // keyboard interceptor remains active to swallow the trailing
        // key-release event (esp. for Esc whose press+release pair is
        // generated synchronously).
        auto pending_unhook = ptr::shared(faux);
        // Shared selection cursor for the button bar.  0 = Confirm (default),
        // 1 = Cancel.  Driven by both Tab (keyboard) and mouse hover (via the
        // overlay MouseMove handler installed below); the most recent input
        // wins.
        auto selected_idx = ptr::shared(si32{ 0 });
        // Hover gate (same pattern as the workspace popup, tile.hpp). The
        // gate emits a per-frame fire(MouseMove) (see console.hpp) so that
        // the first MouseMove the overlay receives reflects the cursor
        // position from BEFORE the dialog opened — using it would let a
        // pre-resting cursor immediately yank selected_idx away from the
        // keyboard default. popup_ready_ptr discards that initial event;
        // last_coord_ptr then suppresses the per-frame echo while the
        // cursor stays put, so keyboard navigation isn't snapped back by a
        // stationary cursor still hovering over a button.
        auto popup_ready_ptr = ptr::shared(faux);
        auto last_coord_ptr  = ptr::shared(twod{});
        // Weak refs to both button items for redraws when selection changes.
        // Populated once the buttons are attached below.
        auto confirm_shadow = ptr::shared(netxs::wptr<ui::base>{});
        auto cancel_shadow  = ptr::shared(netxs::wptr<ui::base>{});
        // Helper: trigger redraw of both buttons (so the highlight follows
        // the selection cursor).
        auto refresh_buttons = [confirm_shadow, cancel_shadow]
        {
            if (auto p = confirm_shadow->lock()) p->base::deface();
            if (auto p = cancel_shadow->lock())  p->base::deface();
        };

        // dismiss_visual: Remove overlay and clear dialog_active flag, but
        //                 keep the keyboard interceptor alive.
        auto dismiss_visual = [overlay_shadow, parent_shadow, pending_unhook]
        {
            if (*pending_unhook) return; // Already dismissed visually.
            *pending_unhook = true;
            if (auto p = parent_shadow.lock())
            {
                p->base::property("msgbox.active", faux) = faux;
            }
            if (auto o = overlay_shadow.lock())
            {
                o->base::detach();
            }
        };
        // dismiss_hook: Unsubscribe the keyboard interceptor from the parent.
        auto dismiss_hook = [kbd_hook]
        {
            kbd_hook->reset();
        };

        // Keyboard interceptor – installed on the PARENT, not the overlay.
        // On tier::preview the dispatch order is feed::rev (general-first):
        //   keybd::any handlers fire BEFORE keybd::post (pro::focus routing).
        // Setting gear.handled prevents pro::focus from delivering the key to
        // the focused child (e.g. ui::term / shell).
        parent.bell::submit(tier::preview, input::events::keybd::any, *kbd_hook)
            = [dismiss_visual, dismiss_hook, on_confirm, on_cancel, pending_unhook,
               selected_idx, refresh_buttons](hids& gear) mutable
        {
            if (gear.payload != input::keybd::type::keypress
                || gear.keystat == input::key::interrupted
                || gear.keybd::handled)
            {
                return;
            }
            // After visual dismiss: swallow the trailing key-release then
            // remove the interceptor entirely.
            if (*pending_unhook)
            {
                gear.set_handled(faux);
                if (gear.keystat == input::key::released)
                {
                    dismiss_hook();
                }
                return;
            }
            // From here on we only act on key-press events.  Key-release
            // events are swallowed unconditionally — letting them through
            // would (a) re-trigger the Tab/Enter handlers on release, and
            // (b) leak releases into the focused child below the overlay.
            if (gear.keystat != input::key::pressed)
            {
                gear.set_handled(faux);
                return;
            }
            auto k = gear.keybd::generic();
            if (k == input::key::Esc)
            {
                dismiss_visual();
                if (on_cancel) on_cancel();
                gear.set_handled(faux);
            }
            else if (k == input::key::Tab)
            {
                // Tab toggles which button is highlighted.  The next Enter
                // will dispatch to whichever button is currently selected.
                *selected_idx = (*selected_idx == 0) ? 1 : 0;
                refresh_buttons();
                gear.set_handled(faux);
            }
            else if (k == input::key::KeyLeftArrow)
            {
                // Left arrow: move selection to Confirm (index 0) — no wrap.
                if (*selected_idx != 0)
                {
                    *selected_idx = 0;
                    refresh_buttons();
                }
                gear.set_handled(faux);
            }
            else if (k == input::key::KeyRightArrow)
            {
                // Right arrow: move selection to Cancel (index 1) — no wrap.
                if (*selected_idx != 1)
                {
                    *selected_idx = 1;
                    refresh_buttons();
                }
                gear.set_handled(faux);
            }
            else if (k == input::key::KeyEnter)
            {
                // Enter dispatches based on the current selection cursor
                // rather than always confirming.
                if (*selected_idx == 0) // Confirm.
                {
                    dismiss_visual();
                    dismiss_hook();
                    on_confirm();
                }
                else // Cancel.
                {
                    dismiss_visual();
                    if (on_cancel) on_cancel();
                }
                gear.set_handled(faux);
            }
            else // Swallow all other keys while dialog is open.
            {
                gear.set_handled(faux);
            }
        };

        // Centralized hover handler (mirrors workspace popup's MouseMove
        // dispatch, tile.hpp). Owns all mouse-driven updates to
        // selected_idx so the gate logic lives in one place instead of
        // racing redirect_mouse_focus → MouseEnter on individual buttons.
        // Hit-test is computed from the overlay size and the dialog_*
        // layout constants defined at the top of this function.
        overlay_ptr->invoke([popup_ready_ptr, last_coord_ptr, selected_idx, refresh_buttons, overlay_shadow, dialog_h, btn_row_y](auto& ovl)
        {
            ovl.on(tier::mouserelease, input::key::MouseMove,
                [popup_ready_ptr, last_coord_ptr, selected_idx, refresh_buttons, overlay_shadow, dialog_h, btn_row_y](hids& gear)
            {
                if (!*popup_ready_ptr)
                {
                    *popup_ready_ptr = true;
                    *last_coord_ptr  = gear.coord;
                    return;
                }
                if (gear.coord == *last_coord_ptr) return; // Stationary cursor echo — suppress so keyboard wins.
                *last_coord_ptr = gear.coord;

                auto ovl_ptr = overlay_shadow.lock();
                if (!ovl_ptr) return;
                auto full = ovl_ptr->base::area().size;
                auto dx = (full.x - dialog_w) / 2; // Dialog top-left in overlay coords.
                auto dy = (full.y - dialog_h) / 2;
                auto mx = (si32)gear.coord.x - dx;
                auto my = (si32)gear.coord.y - dy;
                auto new_sel = *selected_idx;
                if (my == btn_row_y)
                {
                    if      (mx >= confirm_x0 && mx < confirm_x1) new_sel = 0;
                    else if (mx >= cancel_x0  && mx < cancel_x1)  new_sel = 1;
                }
                if (new_sel != *selected_idx)
                {
                    *selected_idx = new_sel;
                    refresh_buttons();
                }
            });
        });

        // Layer 1: Dimming scrim (click outside → cancel).
        overlay_ptr->attach(ui::mock::ctor())
            ->invoke([dismiss_visual, dismiss_hook, on_cancel](auto& boss)
            {
                auto myid = boss.bell::id;
                boss.LISTEN(tier::release, e2::render::background::any, parent_canvas, -, (myid))
                {
                    parent_canvas.fill([myid](cell& c)
                    {
                        c.bgc().faint();
                        c.fgc().faint();
                        c.cur(text_cursor::none); // Suppress any terminal cursor bleeding through.
                        c.link(myid);
                    });
                };
                boss.on(tier::mouserelease, input::key::LeftClick, [&, dismiss_visual, dismiss_hook, on_cancel](hids& gear)
                {
                    dismiss_visual();
                    dismiss_hook();
                    if (on_cancel) on_cancel();
                    gear.dismiss();
                });
            });

        // Layer 2: Centered dialog card (Tokyo Night palette). Geometry is
        // governed by the dialog_* constants at the top of this function;
        // see the ascii diagram there.
        auto dialog = overlay_ptr->attach(ui::fork::ctor(axis::Y))
            ->alignment({ snap::center, snap::center })
            ->limits({ dialog_w, dialog_h }, { dialog_w, dialog_h })
            ->invoke([](auto& boss)
            {
                // Reset every attribute in the dialog rect before colors()
                // paints the bg. Without this, fusefull would blend the dialog
                // colors over whatever style bits (italic/bold/underline/etc.)
                // the dimmed underlying content already carried — those style
                // bits would bleed through the confirm dialog card.
                boss.LISTEN(tier::release, e2::render::background::any, parent_canvas, -)
                {
                    parent_canvas.fill(cell::shaders::wipe);
                };
            })
            ->colors(argb{ 0xffc0caf5 }, argb{ 0xff1a1b26 })
            ->setpad({ pad_l, pad_r, pad_t, pad_b });

        // Message label — flexible keeps full slot width.
        // No alignment() here: the Y-fork inform() bug yields zero-size
        // regions for children; center alignment would shift the item
        // off-screen.  Blue accent (0xff7aa2f7) for the question text.
        // ui::item disables wrapping by default (controls.hpp _set() forces
        // wrap::off). Re-enable it via an inline wrp(wrap::on) directive so
        // long messages flow into multiple lines instead of being truncated.
        dialog->attach(slot::_1,
                ui::item::ctor(ansi::wrp(wrap::on)
                    .fgc(0xffcdd6f4)
                    .add(texts.message)))
            ->flexible();

        // Button bar: outer takes btn_row_h + gap_h rows, with gap_h rows
        // of top padding so the actual buttons sit on the bottom row and a
        // gap_h-tall empty strip separates them from the (possibly wrapped)
        // message above.
        auto buttons = dialog->attach(slot::_2, ui::fork::ctor(axis::X, btn_grip))
            ->limits({ -1, btn_row_h + gap_h }, { -1, btn_row_h + gap_h })
            ->setpad({ 0, 0, gap_h, 0 });

        // Button color scheme — matches the command search bar's vertical
        // scrollbar thumb:
        //   * idle  : bg = scroll_thumb (0xFF3B4261), fg = light text
        //   * active: bg = scroll_drag  (0xFF89B4FA), fg = dark  text
        // The "active" treatment fires both for keyboard selection and for
        // mouse hover, since both update *selected_idx.
        static constexpr auto btn_idle_bg     = argb{ 0xff3b4261 };
        static constexpr auto btn_idle_fg     = argb{ 0xffcdd6f4 };
        static constexpr auto btn_active_bg   = argb{ 0xff89b4fa };
        static constexpr auto btn_active_fg   = argb{ 0xff1e1e2e };
        // Note: use a default-constructed cell (no glyph, no mosaic metadata)
        // so that fusefull only blends fg/bg into existing cells without
        // overwriting the label glyphs (gc) — otherwise the active-state
        // re-paint would replace the rendered text with whitespace.
        auto active_brush = cell{}.fgc(btn_active_fg).bgc(btn_active_bg);

        // Center button labels within the btn_w-column button cell.
        auto pad_label = [](view label) -> text
        {
            auto len = (si32)utf::length(label);
            if (len >= btn_w) return text{ label };
            auto total = btn_w - len;
            auto left  = total / 2;
            auto right = total - left;
            return text(left, ' ').append(label).append(right, ' ');
        };
        auto confirm_label_padded = pad_label(texts.confirm_label);
        auto cancel_label_padded  = pad_label(texts.cancel_label);

        // [ Confirm ] button.
        auto confirm_btn = buttons->attach(slot::_1, ui::item::ctor(ansi::fgc(btn_idle_fg).add(confirm_label_padded)))
            ->active(btn_idle_fg, btn_idle_bg)
            ->invoke([dismiss_visual, dismiss_hook, on_confirm, selected_idx, active_brush](auto& boss)
            {
                // Render-time highlight: paint the active palette on top of
                // the idle one when this button is the current selection.
                // selected_idx is driven by Tab (keyboard) and by the
                // overlay MouseMove handler (mouse hover); whichever input
                // arrived last wins.
                //
                // We listen on the foreground render pass (e2::render::any)
                // rather than the background pass, so that the active brush
                // overpaints the label glyphs themselves — otherwise the
                // glyph foreground (baked in via ansi::fgc(btn_idle_fg) on
                // the label content) would render on top of the active
                // brush and btn_active_fg would never take effect.
                boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (selected_idx, active_brush))
                {
                    if (*selected_idx == 0)
                    {
                        parent_canvas.fill(cell::shaders::fusefull(active_brush));
                    }
                };
                boss.on(tier::mouserelease, input::key::LeftClick, [&, dismiss_visual, dismiss_hook, on_confirm](hids& gear)
                {
                    dismiss_visual();
                    dismiss_hook();
                    on_confirm();
                    gear.dismiss();
                });
            });
        *confirm_shadow = ptr::shadow(confirm_btn);

        // [ Cancel ] button.
        auto cancel_btn = buttons->attach(slot::_2, ui::item::ctor(ansi::fgc(btn_idle_fg).add(cancel_label_padded)))
            ->active(btn_idle_fg, btn_idle_bg)
            ->invoke([dismiss_visual, dismiss_hook, on_cancel, selected_idx, active_brush](auto& boss)
            {
                boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (selected_idx, active_brush))
                {
                    if (*selected_idx == 1)
                    {
                        parent_canvas.fill(cell::shaders::fusefull(active_brush));
                    }
                };
                boss.on(tier::mouserelease, input::key::LeftClick, [&, dismiss_visual, dismiss_hook, on_cancel](hids& gear)
                {
                    dismiss_visual();
                    dismiss_hook();
                    if (on_cancel) on_cancel();
                    gear.dismiss();
                });
            });
        *cancel_shadow = ptr::shadow(cancel_btn);

        // Attach the overlay to the parent and trigger layout.
        parent.attach(overlay_ptr);
        parent.base::reflow();
    }
    // closing_on_quit_with_confirm: Like closing_on_quit but optionally shows
    // a confirmation dialog before actually closing.
    const auto closing_on_quit_with_confirm = [](auto& boss, bool confirm)
    {
        if (!confirm)
        {
            closing_on_quit(boss);
            return;
        }
        boss.LISTEN(tier::anycast, e2::form::proceed::quit::any, fast)
        {
            if (fast) // Fast close (e.g. session disconnect) – skip confirmation.
            {
                boss.base::riseup(tier::release, e2::form::proceed::quit::one, fast);
            }
            else // User-initiated close – show confirmation dialog.
            {
                show_close_confirmation(boss, [&boss]
                {
                    boss.base::riseup(tier::release, e2::form::proceed::quit::one, true);
                });
            }
        };
    };
    const auto scroll_bars = [](auto master)
    {
        auto sb = ui::fork::ctor();
        auto bt = sb->attach(slot::_1, ui::fork::ctor(axis::Y));
        auto hz = bt->attach(slot::_2, ui::grip<axis::X>::ctor(master));
        auto vt = sb->attach(slot::_2, ui::grip<axis::Y>::ctor(master));
        return sb;
    };
    const auto underlined_hz_scrollbar = [](auto scrlrail)
    {
        auto grip = ui::grip<axis::X>::ctor(scrlrail, ui::drawfx::underline)
            ->alignment({ snap::both, snap::tail })
            ->invoke([&](auto& boss)
            {
                boss.base::hidden = true;
                scrlrail->LISTEN(tier::release, e2::form::state::mouse, hovered, -, (grip_shadow = ptr::shadow(boss.This())))
                {
                    if (auto grip_ptr = grip_shadow.lock())
                    {
                        grip_ptr->base::hidden = !hovered;
                        grip_ptr->base::reflow();
                    }
                };
            });
        return grip;
    };
    const auto scroll_bars_term = [](auto master)
    {
        auto scroll_bars = ui::fork::ctor();
        auto scroll_head = scroll_bars->attach(slot::_1, ui::fork::ctor(axis::Y));
        auto hz = scroll_head->attach(slot::_1, ui::grip<axis::X>::ctor(master));
        auto vt = scroll_bars->attach(slot::_2, ui::grip<axis::Y>::ctor(master));
        return scroll_bars;
    };
    const auto set_title = [](base& boss, input::hids& gear, bias alignment = bias::left)
    {
        auto old_title = boss.base::riseup(tier::request, e2::form::prop::ui::header);
        gear.owner.base::signal(tier::request, input::events::clipboard, gear);
        auto& data = gear.board::cargo;
        if (data.utf8.empty())
        {
            log("%%Clipboard is empty or contains non-text data", prompt::desk);
        }
        else
        {
            if (utf::is_plain(data.utf8) || alignment != bias::left) // Reset aligning to the center if text is plain.
            {
                auto align = ansi::jet(alignment);
                boss.base::riseup(tier::preview, e2::form::prop::ui::header, align);
            }
            // Copy clipboard data to title.
            auto title = data.utf8;
            boss.base::riseup(tier::preview, e2::form::prop::ui::header, title);
            if (old_title.size()) // Copy old title to clipboard.
            {
                gear.set_clipboard(dot_00, old_title, mime::ansitext);
            }
        }
    };
    const auto base_kb_navigation = [](settings& config, ui::sptr scroll_ptr, base& boss)
    {
        auto& scroll_inst = *scroll_ptr;
        boss.base::plugin<pro::keybd>();
        auto& luafx = boss.bell::indexer.luafx;
        auto defapp_context = config.settings::push_context("/config/events/defapp/");
        auto script_list = config.settings::take_ptr_list_for_name("script");
        auto bindings = input::bindings::load(config, script_list);
        input::bindings::keybind(boss, bindings);
        boss.base::add_methods(basename::defapp,
        {
            { "ScrollViewportByPage",   [&]
                                        {
                                            auto step = twod{ luafx.get_args_or(1, 0),
                                                              luafx.get_args_or(2, 0) };
                                            scroll_inst.base::riseup(tier::preview, e2::form::upon::scroll::bypage::v, { .vector = step });
                                            luafx.get_gear().set_handled();
                                            luafx.set_return(); // No returns.
                                        }},
            { "ScrollViewportByStep",   [&]
                                        {
                                            auto step = twod{ luafx.get_args_or(1, 0),
                                                              luafx.get_args_or(2, 0) };
                                            scroll_inst.base::riseup(tier::preview, e2::form::upon::scroll::bystep::v, { .vector = step });
                                            luafx.get_gear().set_handled();
                                            luafx.set_return(); // No returns.
                                        }},
            { "ScrollViewportToTop",    [&]
                                        {
                                            scroll_inst.base::riseup(tier::preview, e2::form::upon::scroll::to_top::y);
                                            luafx.get_gear().set_handled();
                                            luafx.set_return(); // No returns.
                                        }},
            { "ScrollViewportToEnd",    [&]
                                        {
                                            scroll_inst.base::riseup(tier::preview, e2::form::upon::scroll::to_end::y);
                                            luafx.get_gear().set_handled();
                                            luafx.set_return(); // No returns.
                                        }},
            { "ShowClosingPreview",     [&]
                                        {
                                            auto& closing_preview_state = boss.base::property("defapp.closing_preview_state", faux);
                                            auto args_count = luafx.args_count();
                                            if (args_count && std::exchange(closing_preview_state, luafx.get_args_or(1, faux)) != closing_preview_state)
                                            {
                                                boss.base::signal(tier::anycast, e2::form::state::keybd::command::close, closing_preview_state);
                                            }
                                            luafx.set_return(closing_preview_state);
                                        }},
            { "Close",                  [&]
                                        {
                                            boss.base::enqueue([](auto& boss) // Keep the focus tree intact while processing events.
                                            {
                                                boss.base::riseup(tier::release, e2::form::proceed::quit::one, true);
                                            });
                                            luafx.get_gear().set_handled();
                                            luafx.set_return();
                                        }},
        });
    };
    const auto applet_kb_navigation = [](settings& config, ui::sptr boss_ptr)
    {
        auto& boss = *boss_ptr;
        boss.base::plugin<pro::focus>();
        boss.base::plugin<pro::keybd>();
        auto& luafx = boss.bell::indexer.luafx;
        auto& bindings = boss.base::property<input::bindings::vector>("applet.bindings");
        auto applet_context = config.settings::push_context("/config/events/applet/");
        auto script_list = config.settings::take_ptr_list_for_name("script");
        bindings = input::bindings::load(config, script_list);
        input::bindings::keybind(boss, bindings);
        boss.base::add_methods(basename::applet,
        {
            { "GetTitlesHeight",    [&]
                                    {
                                        auto h = 0;
                                        auto f = 0;
                                        if (auto parent_ptr = boss.base::parent())
                                        {
                                            parent_ptr->base::if_plugin<pro::title>([&](auto& title)
                                            {
                                                h = title.head_size.y;
                                                f = title.foot_size.y;
                                            });
                                        }
                                        luafx.set_return(h, f);
                                    }},
            { "GetArea",            [&]
                                    {
                                        if (auto parent_ptr = boss.base::parent()) // Try to get window area first.
                                        {
                                            auto area = parent_ptr->base::area();
                                            luafx.set_return(area);
                                        }
                                        else
                                        {
                                            luafx.set_return(boss.base::area());
                                        }
                                    }},
            { "Warp",               [&]
                                    {
                                        auto gui_cmd = e2::command::gui.param();
                                        auto& gear = luafx.get_gear();
                                        if (gear.is_real())
                                        {
                                            gui_cmd.gear_id = gear.id;
                                            gear.set_handled();
                                        }
                                        gui_cmd.cmd_id = syscmd::warpwindow;
                                        gui_cmd.args.emplace_back(luafx.get_args_or(1, si32{ 0 }));
                                        gui_cmd.args.emplace_back(luafx.get_args_or(2, si32{ 0 }));
                                        gui_cmd.args.emplace_back(luafx.get_args_or(3, si32{ 0 }));
                                        gui_cmd.args.emplace_back(luafx.get_args_or(4, si32{ 0 }));
                                        boss.base::riseup(tier::preview, e2::command::gui, gui_cmd);
                                        luafx.set_return();
                                    }},
            { "ZOrder",            [&]
                                    {
                                        auto args_count = luafx.args_count();
                                        auto& zorder = boss.base::property("applet.zorder", zpos::plain);
                                        auto& gear = luafx.get_gear();
                                        if (args_count)
                                        {
                                            auto gui_cmd = e2::command::gui.param();
                                            zorder = luafx.get_args_or(1, zpos::plain);
                                            if (gear.is_real())
                                            {
                                                gui_cmd.gear_id = gear.id;
                                            }
                                            gui_cmd.cmd_id = syscmd::zorder;
                                            gui_cmd.args.emplace_back(zorder);
                                            boss.base::riseup(tier::release, e2::form::prop::zorder, si32{ zorder });
                                            boss.base::riseup(tier::preview, e2::command::gui, gui_cmd);
                                        }
                                        gear.set_handled();
                                        luafx.set_return(zorder);
                                    }},
            { "State",              [&]
                                    {
                                        auto state = boss.base::riseup(tier::request, e2::form::prop::window::state);
                                        switch (state)
                                        {
                                            default:                   luafx.set_return("undefined");  break;
                                            case winstate::normal:     luafx.set_return("normal");     break;
                                            case winstate::minimized:  luafx.set_return("minimized");  break;
                                            case winstate::maximized:  luafx.set_return("maximized");  break;
                                            case winstate::fullscreen: luafx.set_return("fullscreen"); break;
                                            case winstate::tiled:      luafx.set_return("tiled");      break;
                                        }
                                    }},
            { "Close",              [&]
                                    {
                                        auto gui_cmd = e2::command::gui.param();
                                        auto& gear = luafx.get_gear();
                                        if (gear.is_real())
                                        {
                                            gui_cmd.gear_id = gear.id;
                                            gear.set_handled();
                                        }
                                        gui_cmd.cmd_id = syscmd::close;
                                        boss.base::riseup(tier::preview, e2::command::gui, gui_cmd);
                                        luafx.set_return();
                                    }},
            { "Minimize",           [&]
                                    {
                                        auto gui_cmd = e2::command::gui.param();
                                        auto& gear = luafx.get_gear();
                                        if (gear.is_real())
                                        {
                                            gui_cmd.gear_id = gear.id;
                                            gear.set_handled();
                                        }
                                        gui_cmd.cmd_id = syscmd::minimize;
                                        boss.base::riseup(tier::preview, e2::command::gui, gui_cmd);
                                        luafx.set_return();
                                    }},
            { "Maximize",           [&]
                                    {
                                        auto gui_cmd = e2::command::gui.param();
                                        auto& gear = luafx.get_gear();
                                        if (gear.is_real())
                                        {
                                            gui_cmd.gear_id = gear.id;
                                            gear.set_handled();
                                        }
                                        gui_cmd.cmd_id = syscmd::maximize;
                                        boss.base::riseup(tier::preview, e2::command::gui, gui_cmd);
                                        luafx.set_return();
                                    }},
            { "Fullscreen",         [&]
                                    {
                                        auto gui_cmd = e2::command::gui.param();
                                        auto& gear = luafx.get_gear();
                                        if (gear.is_real())
                                        {
                                            gui_cmd.gear_id = gear.id;
                                            gear.set_handled();
                                        }
                                        gui_cmd.cmd_id = syscmd::fullscreen;
                                        boss.base::riseup(tier::preview, e2::command::gui, gui_cmd);
                                        luafx.set_return();
                                    }},
            { "Restore",            [&]
                                    {
                                        auto gui_cmd = e2::command::gui.param();
                                        auto& gear = luafx.get_gear();
                                        if (gear.is_real())
                                        {
                                            gui_cmd.gear_id = gear.id;
                                            gear.set_handled();
                                        }
                                        gui_cmd.cmd_id = syscmd::restore;
                                        boss.base::riseup(tier::preview, e2::command::gui, gui_cmd);
                                        luafx.set_return();
                                    }},
        });
    };

    using builder_t = std::function<ui::sptr(eccc, settings&)>;

    namespace win
    {
        namespace type
        {
            static const auto undefined  = "undefined"s;
            static const auto normal     = "normal"s;
            static const auto minimized  = "minimized"s;
            static const auto maximized  = "maximized"s;
            static const auto fullscreen = "fullscreen"s;
            static const auto tiled      = "tiled"s;
        }

        static auto options = utf::unordered_map<text, si32>
           {{ type::undefined,  winstate::normal     },
            { type::normal,     winstate::normal     },
            { type::minimized,  winstate::minimized  },
            { type::maximized,  winstate::maximized  },
            { type::fullscreen, winstate::fullscreen },
            { type::tiled,      winstate::tiled      }};
    }

    namespace menu
    {
        struct item
        {
            bool alive{};
            text label{};
            text tooltip{};
            cell hover{};
            cell focus{};
            input::bindings::vector bindings;
        };

        using action_map_t = utf::unordered_map<text, std::function<void(ui::item&, menu::item&)>>;
        using link = std::tuple<item, std::function<void(ui::item&, item&)>>;
        using list = std::list<link>;

        static auto mini(bool autohide, bool slimsize, si32 custom, list menu_items) // Menu bar (shrinkable on right-click).
        {
            //auto highlight_color = skin::color(tone::highlight);
            auto danger_color    = skin::color(tone::danger);
            //auto action_color    = skin::color(tone::action);
            //auto warning_color   = skin::color(tone::warning);
            //auto c6 = action_color;
            //auto c3 = highlight_color;
            //auto c2 = warning_color;
            auto c1 = danger_color;
            auto macstyle = skin::globals().macstyle;
            auto menuveer = ui::veer::ctor();
            auto menufork = ui::fork::ctor(axis::X, 0, macstyle ? 0 : 1, macstyle ? 1 : 0)
                ->active();
            auto makeitem = [&](auto& config)
            {
                auto& props = std::get<0>(config);
                auto& setup = std::get<1>(config);
                auto& alive = props.alive;
                auto& label = props.label;
                auto& tooltip = props.tooltip;
                auto& hover = props.hover;
                auto button = ui::item::ctor(label)->drawdots();
                button->active(); // Always active for tooltips.
                if (alive)
                {
                    if (hover.clr()) button->shader(cell::shaders::mimic(hover), e2::form::state::hover);
                    else             button->shader(cell::shaders::xlight,       e2::form::state::hover);
                }
                button->template plugin<pro::notes>(tooltip)
                    ->setpad({ 0, 0, !slimsize, !slimsize })
                    ->invoke([&](auto& boss) // Store shared ptr to the menu item config.
                    {
                        auto& item_props = boss.base::field(std::move(props));
                        setup(boss, item_props);
                    });
                return button;
            };
            auto ctrlslot = macstyle ? slot::_1 : slot::_2;
            auto menuslot = macstyle ? slot::_2 : slot::_1;
            auto ctrllist = menufork->attach(ctrlslot, ui::list::ctor(axis::X)
                ->alignment({ snap::both, snap::both }));
            if (custom) // Apply a custom menu controls.
            {
                while (custom--)
                {
                    auto button = makeitem(menu_items.back());
                    ctrllist->attach<sort::reverse>(button);
                    menu_items.pop_back();
                }
            }
            else // Add standard menu controls.
            {
                auto control = std::vector<link>
                {
                    { menu::item{ .alive = true, .label = "  —  ", .tooltip = skin::globals().NsMinimizeWindow_tooltip },//, .hover = c2 }, //todo too funky
                    [](auto& boss, auto& /*item*/)
                    {
                        boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
                        {
                            boss.base::riseup(tier::preview, e2::form::size::minimize, gear);
                            gear.break_click_chain();
                            gear.dismiss();
                        });
                    }},
                    { menu::item{ .alive = true, .label = "  □  ", .tooltip = skin::globals().NsMaximizeWindow_tooltip },//, .hover = c6 },
                    [](auto& boss, auto& /*item*/)
                    {
                        auto sync = [](auto& boss, auto state)
                        {
                            auto restore = state == winstate::maximized
                                        || state == winstate::fullscreen;
                            auto label = restore ? "  ▣  "sv
                                                 : "  □  "sv;
                            if (boss.get() != label)
                            {
                                boss.set(label);
                                boss.base::deface();
                            }
                        };
                        auto state_watch_ptr = &boss.base::field(hook{});
                        boss.LISTEN(tier::anycast, e2::form::upon::started, root_ptr, -, (sync, state_watch_ptr))
                        {
                            auto& state_watch = *state_watch_ptr;
                            state_watch = {};
                            auto state = boss.base::riseup(tier::request, e2::form::prop::window::state);
                            sync(boss, state);
                            auto target = boss.base::riseup(tier::request, e2::form::prop::window::statesrc);
                            if (target)
                            {
                                auto button_shadow = ptr::shadow(boss.This());
                                target->LISTEN(tier::release, e2::form::prop::window::state, state, state_watch, (button_shadow, sync))
                                {
                                    if (auto button_ptr = button_shadow.lock())
                                    {
                                        sync(*button_ptr, state);
                                    }
                                };
                            }
                        };
                        boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
                        {
                            boss.base::riseup(tier::preview, e2::form::size::enlarge::maximize, gear);
                            gear.break_click_chain();
                            gear.dismiss();
                        });
                    }},
                    { menu::item{ .alive = true, .label = "  ×  ", .tooltip = skin::globals().NsCloseWindow_tooltip, .hover = c1 },
                    [c1](auto& boss, auto& /*item*/)
                    {
                        boss.template shader<tier::anycast>(cell::shaders::color(c1), e2::form::state::keybd::command::close);
                        boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
                        {
                            // Focus the pane so its menubar shows active palette during confirmation.
                            if (auto win_ptr = boss.base::riseup(tier::request, e2::form::prop::window::statesrc))
                            {
                                pro::focus::set(win_ptr, gear.id, solo::on);
                            }
                            auto backup = boss.This(); //todo revise backup
                            boss.base::signal(tier::anycast, e2::form::proceed::quit::one, faux); // fast=faux: Show closing process.
                            gear.break_click_chain();
                            gear.dismiss();
                        });
                    }},
                };
                if (macstyle)
                {
                    ctrllist->attach(makeitem(control[2]));
                    ctrllist->attach(makeitem(control[0]));
                    ctrllist->attach(makeitem(control[1]));
                }
                else
                {
                    ctrllist->attach(makeitem(control[0]));
                    ctrllist->attach(makeitem(control[1]));
                    ctrllist->attach(makeitem(control[2]));
                }
            }
            auto scrlarea = menufork->attach(menuslot, ui::cake::ctor()
                ->limits({ 0, -1 }) // Allow the menu area to shrink to zero so ctrllist stays at the right edge.
                ->alignment({ snap::head, snap::both })); // Don't propagate "need more width" upstream so fork can give scrlarea negative split for narrow terminals.
            auto scrlrail = scrlarea->attach(ui::rail::ctor(axes::X_only, axes::all));
            auto scrllist = scrlrail->attach(ui::list::ctor(axis::X))
                ->invoke([&](auto& boss)
                {
                    boss.LISTEN(tier::release, e2::postrender, parent_canvas) // Draw a shadow to split button groups.
                    {
                        auto clip = parent_canvas.clip();
                        auto full = parent_canvas.full();
                        if (clip.coor.x + clip.size.x < full.coor.x + full.size.x)
                        {
                            auto vert_line = clip;
                            vert_line.coor.x += vert_line.size.x - 1;
                            vert_line.size.x = 1;
                            parent_canvas.fill(vert_line, cell::shaders::shadow(ui::pro::ghost::x3y1_x3y2_x3y3));
                        }
                        if (clip.coor.x > full.coor.x)
                        {
                            auto vert_line = clip;
                            vert_line.size.x = 1;
                            parent_canvas.fill(vert_line, cell::shaders::shadow(ui::pro::ghost::x1y1_x1y2_x1y3));
                        }
                    };
                });

            auto scrlcake = ui::cake::ctor();
            auto scrlhint = scrlcake->attach(underlined_hz_scrollbar(scrlrail));
            auto scrlgrip = scrlarea->attach(scrlcake);

            for (auto& body : menu_items)
            {
                scrllist->attach(makeitem(body));
            }

            auto menucake = menuveer->attach(ui::cake::ctor()->branch(menufork))
                ->invoke([&](auto& boss)
                {
                    auto& slim_status = boss.base::field(slimsize);
                    boss.LISTEN(tier::anycast, e2::form::upon::resized, new_area)
                    {
                        if (!slim_status)
                        {
                            auto height = boss.base::min_sz.y;
                            if (new_area.size.y < 3)
                            {
                                if (height != new_area.size.y)
                                {
                                    boss.base::limits({ -1, new_area.size.y }, { -1, new_area.size.y });
                                }
                            }
                            else if (height != 3)
                            {
                                boss.base::limits({ -1, 3 }, { -1, 3 });
                            }
                        }
                    };
                    boss.LISTEN(tier::anycast, e2::form::prop::ui::slimmenu, slim)
                    {
                        slim_status = slim;
                        auto height = slim ? 1 : 3;
                        boss.base::limits({ -1, height }, { -1, height });
                        boss.reflow();
                    };
                });
            auto menutent = menuveer->attach(ui::mock::ctor()->limits({ -1,1 }, { -1,1 }));
            if (autohide == faux) menuveer->roll();
            menuveer->limits({ -1, slimsize ? 1 : 3 }, { -1, slimsize ? 1 : 3 })
                ->invoke([&](auto& boss)
                {
                    if (autohide)
                    {
                        auto menutent_shadow = ptr::shadow(menutent);
                        auto menucake_shadow = ptr::shadow(menucake);
                        boss.LISTEN(tier::release, e2::form::state::mouse, hovered, -, (menucake_shadow, menutent_shadow))
                        {
                            if (auto menucake = menucake_shadow.lock())
                            {
                                auto menu_visible = boss.back() != menucake;
                                if (hovered == menu_visible)
                                {
                                    boss.roll();
                                    boss.reflow();
                                    if (auto menutent = menutent_shadow.lock())
                                    {
                                        menutent->base::signal(tier::release, e2::form::state::visible, menu_visible);
                                    }
                                }
                            }
                        };
                    }
                });

            return std::tuple{ menuveer, menutent, menucake };
        };
        const auto create = [](settings& config, list menu_items)
        {
            auto menu_context = config.settings::push_context("menu/");
            auto autohide = config.settings::take("autohide", faux);
            auto slimsize = config.settings::take("slim"    , true);
            return mini(autohide, slimsize, 0, menu_items);
        };
        const auto load = [](settings& config)
        {
            auto list = menu::list{};
            auto menu_context = config.settings::push_context("menu/");
            auto menuitem_ptr_list = config.settings::take_ptr_list_for_name("item");
            for (auto menuitem_ptr : menuitem_ptr_list)
            {
                auto item = menu::item{};
                auto menuitem_context = config.settings::push_context(menuitem_ptr); //todo revise
                auto script_list = config.settings::take_ptr_list_of(menuitem_ptr, "script");
                item.alive = script_list.size();
                item.bindings = input::bindings::load(config, script_list);
                auto classname_list = config.settings::take_value_list_of(menuitem_ptr, "id");
                item.label   = config.settings::take_value_from(menuitem_ptr, "label", " "s);
                item.tooltip = config.settings::take_value_from(menuitem_ptr, "tooltip", ""s);
                if (auto color = config.settings::take("hover/bgc", ui32{ 0 })) item.hover.bgc(color);
                if (auto color = config.settings::take("hover/fgc", ui32{ 0 })) item.hover.fgc(color);
                auto setup = [classname_list = std::move(classname_list)](ui::item& boss, menu::item& item)
                {
                    auto& luafx = boss.bell::indexer.luafx;
                    for (auto& classname : classname_list)
                    {
                        boss.bell::indexer.add_base_class(classname, boss);
                    }
                    input::bindings::keybind(boss, item.bindings);
                    boss.base::add_methods(basename::item,
                    {
                        { "Label",      [&]
                                        {
                                            auto args_count = luafx.args_count();
                                            if (args_count) // Set label.
                                            {
                                                auto new_label = luafx.get_args_or(1, "label"s);
                                                boss.set(new_label);
                                                luafx.set_return();
                                            }
                                            else // Get label.
                                            {
                                                auto current_label = boss.get();
                                                luafx.set_return(current_label);
                                            }
                                        }},
                        { "Tooltip",    [&]
                                        {
                                            
                                            auto args_count = luafx.args_count();
                                            if (args_count) // Set tooltip.
                                            {
                                                auto new_tooltip = luafx.get_args_or(1, ""s);
                                                boss.base::signal(tier::preview, e2::form::prop::ui::tooltip, new_tooltip);
                                                luafx.set_return();
                                            }
                                            else // Get tooltip.
                                            {
                                                auto current_tooltip = boss.base::signal(tier::request, e2::form::prop::ui::tooltip);
                                                luafx.set_return(current_tooltip);
                                            }
                                        }},
                        { "Deface",     [&]
                                        {
                                            boss.base::deface();
                                            luafx.set_return();
                                        }},
                    });
                };
                list.push_back({ item, setup });
            }
            return menu::create(config, list);
        };
        const auto demo = [](settings& config)
        {
            //auto highlight_color = skin::color(tone::highlight);
            //auto c3 = highlight_color;
            auto items = list
            {
                { item{ .alive = true, .label = ansi::add("  ").und(true).add("F").nil().add("ile  "), .tooltip = " File menu item " }, [&](auto& /*boss*/, auto& /*item*/){ }},
                { item{ .alive = true, .label = ansi::add("  ").und(true).add("E").nil().add("dit  "), .tooltip = " Edit menu item " }, [&](auto& /*boss*/, auto& /*item*/){ }},
                { item{ .alive = true, .label = ansi::add("  ").und(true).add("V").nil().add("iew  "), .tooltip = " View menu item " }, [&](auto& /*boss*/, auto& /*item*/){ }},
                { item{ .alive = true, .label = ansi::add("  ").und(true).add("D").nil().add("ata  "), .tooltip = " Data menu item " }, [&](auto& /*boss*/, auto& /*item*/){ }},
                { item{ .alive = true, .label = ansi::add("  ").und(true).add("H").nil().add("elp  "), .tooltip = " Help menu item " }, [&](auto& /*boss*/, auto& /*item*/){ }},
            };
            auto [menu, cover, menu_data] = create(config, items);
            return menu;
        };
    }
    namespace
    {
        auto& creator()
        {
            static auto creator = std::map<text, builder_t>{};
            return creator;
        }
    }
    static auto builder(text app_typename)
    {
        auto& map = creator();
        auto it = map.find(app_typename);
        if (it == map.end())
        {
            log("%%Unknown app type - '%app_typename%'", prompt::apps, app_typename);
            it = map.find("invalid");
        }
        auto& builder_proc = it->second;
        return  [&](eccc appcfg, settings& config)
                {
                    auto applet_ptr = builder_proc(appcfg, config);
                    auto& applet = *applet_ptr;
                    applet.LISTEN(tier::anycast, e2::form::upon::started, root_ptr)
                    {
                        applet.base::enqueue([&](auto&) // Enqueue the trigger to window menu update.
                        {
                            applet.base::signal(tier::release, e2::form::upon::started); // Fire a release started event after all initializations.
                        });
                    };
                    return applet_ptr;
                };
    }
    namespace load
    {
        static auto log_load(view src_path)
        {
            log("%%Loading settings from %path%...", prompt::apps, src_path);
        }
        static auto load_from_file(xml::document& config_inst, qiew file_path)
        {
            auto [config_path, config_path_str] = os::path::expand(file_path);
            if (!config_path.empty())
            {
                log_load(config_path_str);
                auto ec = std::error_code{};
                auto config_file = fs::directory_entry{ config_path, ec };
                if (!ec && (config_file.is_regular_file(ec) || config_file.is_symlink(ec)))
                {
                    auto file = std::ifstream{ config_file.path(), std::ios::binary | std::ios::in };
                    if (!file.seekg(0, std::ios::end).fail())
                    {
                        auto size = file.tellg();
                        auto buff = text((size_t)size, '\0');
                        file.seekg(0, std::ios::beg);
                        file.read(buff.data(), size);
                        config_inst.load(buff, config_path.string());
                        log("%%Loaded %count% bytes", prompt::pads, size);
                        return true;
                    }
                }
                log(prompt::pads, "Not found");
            }
            return faux;
        }
        static auto attach_file_list(txts& file_list, xml::document& src_cfg)
        {
            auto file_ptr_list = src_cfg.take_ptr_list<true>("/include");
            if (file_ptr_list.size())
            {
                log("%%Update settings source files from %src%", prompt::apps, src_cfg.page.file);
                for (auto& file_ptr : file_ptr_list)
                {
                    if (file_ptr->base)
                    {
                        file_list.clear();
                    }
                    auto file_path = file_ptr->_concat_values();
                    if (file_path.size())
                    {
                        log("%%%file%", prompt::pads, file_path);
                        file_list.emplace_back(std::move(file_path));
                    }
                }
            }
        }
        static auto overlay_config(xml::document& def_cfg, xml::document& src_cfg)
        {
            if (src_cfg)
            {
                log(prompt::pads, "Merging settings from ", src_cfg.page.file);
                def_cfg.combine_item(src_cfg);
            }
        }
        static void settings(xml::settings& resultant, qiew cliopt, bool print = faux)
        {
            static auto defaults = utf::replace_all(
                #include "../../vtm.xml"
                , "\n\n", "\n");
            auto envopt = os::env::get("VTM_CONFIG");
            auto defcfg = xml::document{ defaults };
            auto envcfg = xml::document{};
            auto dvtcfg = xml::document{};
            auto clicfg = xml::document{};
            auto file_list = txts{};

            auto show_cfg = [&](auto& cfg){ if (print && cfg) log("%source%:\n%config%", cfg.page.file, cfg.page.show()); };

            if (envopt.size()) // Load settings from the environment variable (plain xml data or file path).
            {
                log_load("$VTM_CONFIG=" + envopt);
                if (envopt.starts_with("<")) // The case with a plain xml data.
                {
                    envcfg.load(envopt, "settings from the environment variable");
                }
                else
                {
                    load_from_file(envcfg, envopt); // The case with a file path.
                }
                show_cfg(envcfg);
            }

            if (os::dtvt::config.size()) // Load settings from the received directvt packet.
            {
                log_load("the received DirectVT packet");
                dvtcfg.load(os::dtvt::config, "dtvt");
                show_cfg(dvtcfg);
            }

            if (cliopt.size()) // Load settings from the specified '-c' cli option (memory mapped source, plain xml data or file path).
            {
                log_load(utf::concat("the specified '-c ", cliopt, "'"));
                if (cliopt.starts_with("<")) // The case with a plain xml data.
                {
                    clicfg.load(cliopt, "settings from the specified '-c' cli option");
                }
                else if (cliopt.starts_with(":")) // Receive configuration via memory mapping.
                {
                    cliopt.remove_prefix(1);
                    auto utf8 = os::process::memory::get(cliopt);
                    clicfg.load(utf8, cliopt);
                }
                else load_from_file(clicfg, cliopt); // The case with a file path.
                show_cfg(clicfg);
            }

            attach_file_list(file_list, defcfg);
            attach_file_list(file_list, envcfg);
            attach_file_list(file_list, dvtcfg);
            attach_file_list(file_list, clicfg);

            for (auto& file_path : file_list) // Overlay configs from the specified sources if it is.
            {
                auto src_conf = xml::document{};
                load_from_file(src_conf, file_path);
                show_cfg(src_conf);
                overlay_config(defcfg, src_conf);
            }

            overlay_config(defcfg, envcfg);
            overlay_config(defcfg, dvtcfg);
            overlay_config(defcfg, clicfg);

            resultant.document.swap(defcfg);
            input::key::kmap::strict_modifiers_only(resultant.settings::take("/config/input/strictchords", faux));
        }
    }

    struct initialize
    {
        initialize(text app_typename, builder_t builder)
        {
            auto& map = creator();
            map[app_typename] = builder;
        }
    };
    static auto get_tui_config(settings& config, ui::skin& g)
    {
        using namespace std::chrono;
        os::dtvt::wheelrate = config.settings::take("/config/timings/wheelrate", 3);
        g.window_clr     = config.settings::take("/config/colors/window"     , cell{ whitespace });
        g.winfocus       = config.settings::take("/config/colors/focus"      , cell{ whitespace });
        g.brighter       = config.settings::take("/config/colors/brighter"   , cell{ whitespace });
        g.shadower       = config.settings::take("/config/colors/shadower"   , cell{ whitespace });
        g.warning        = config.settings::take("/config/colors/warning"    , cell{ whitespace });
        g.danger         = config.settings::take("/config/colors/danger"     , cell{ whitespace });
        g.action         = config.settings::take("/config/colors/action"     , cell{ whitespace });
        g.selected       = config.settings::take("/config/desktop/taskbar/colors/selected"  , cell{ whitespace });
        g.active         = config.settings::take("/config/desktop/taskbar/colors/active"    , cell{ whitespace });
        g.focused        = config.settings::take("/config/desktop/taskbar/colors/focused"   , cell{ whitespace });
        g.inactive       = config.settings::take("/config/desktop/taskbar/colors/inactive"  , cell{ whitespace });
        g.spd            = config.settings::take("/config/timings/kinetic/spd"      , 10  );
        g.pls            = config.settings::take("/config/timings/kinetic/pls"      , 167 );
        g.spd_accel      = config.settings::take("/config/timings/kinetic/spd_accel", 1   );
        g.spd_max        = config.settings::take("/config/timings/kinetic/spd_max"  , 100 );
        g.ccl            = config.settings::take("/config/timings/kinetic/ccl"      , 120 );
        g.ccl_accel      = config.settings::take("/config/timings/kinetic/ccl_accel", 30  );
        g.ccl_max        = config.settings::take("/config/timings/kinetic/ccl_max"  , 1   );
        g.switching      = config.settings::take("/config/timings/switching"        , span{ 200ms });
        g.deceleration   = config.settings::take("/config/timings/deceleration"     , span{ 2s    });
        g.blink_period   = config.settings::take("/config/cursor/blink"             , span{ 400ms });
        g.menu_timeout   = config.settings::take("/config/desktop/taskbar/timeout"  , span{ 250ms });
        g.leave_timeout  = config.settings::take("/config/timings/leave_timeout"    , span{ 1s    });
        g.repeat_delay   = config.settings::take("/config/timings/repeat_delay"     , span{ 500ms });
        g.repeat_rate    = config.settings::take("/config/timings/repeat_rate"      , span{ 30ms  });
        g.maxfps         = config.settings::take("/config/timings/fps"              , 60);
        g.max_value      = config.settings::take("/config/desktop/windowmax"        , twod{ 3000, 2000  });
        g.macstyle       = config.settings::take("/config/desktop/macstyle"         , faux);
        g.menuwide       = config.settings::take("/config/desktop/taskbar/wide"     , faux);
        if (g.maxfps <= 0) g.maxfps = 60;

        g.NsTextbasedDesktopEnvironment   = config.settings::take("/Ns/TextbasedDesktopEnvironment"    , ""s);
        g.NsInfo_label                    = config.settings::take("/Ns/Info/label"                     , ""s);
        g.NsInfo_tooltip                  = config.settings::take("/Ns/Info/tooltip"                   , ""s);
        g.NsInfo_title                    = config.settings::take("/Ns/Info/title"                     , ""s);
        g.NsInfoKeybdTest                 = config.settings::take("/Ns/Info/KeybdTest"                 , ""s);
        g.NsInfoKeybdMode                 = config.settings::take("/Ns/Info/KeybdMode"                 , ""s);
        g.NsInfoKeybdToggle_on            = config.settings::take("/Ns/Info/KeybdToggle/on"            , ""s);
        g.NsInfoKeybdToggle_off           = config.settings::take("/Ns/Info/KeybdToggle/off"           , ""s);
        g.NsInfo_pressed                  = config.settings::take("/Ns/Info/pressed"                   , ""s);
        g.NsInfo_released                 = config.settings::take("/Ns/Info/released"                  , ""s);
        g.NsInfo_pressanykeys             = config.settings::take("/Ns/Info/pressanykeys"              , ""s);
        g.NsInfoGeneric                   = config.settings::take("/Ns/Info/Generic"                   , ""s);
        g.NsInfoLiteral                   = config.settings::take("/Ns/Info/Literal"                   , ""s);
        g.NsInfoSpecific                  = config.settings::take("/Ns/Info/Specific"                  , ""s);
        g.NsInfoScancodes                 = config.settings::take("/Ns/Info/Scancodes"                 , ""s);
        g.NsInfo_copied                   = config.settings::take("/Ns/Info/copied"                    , ""s);
        g.NsInfoStatus                    = config.settings::take("/Ns/Info/Status"                    , ""s);
        g.NsInfoSystem                    = config.settings::take("/Ns/Info/System"                    , ""s);
        g.NsInfoYes                       = config.settings::take("/Ns/Info/Yes"                       , ""s);
        g.NsInfoNo                        = config.settings::take("/Ns/Info/No"                        , ""s);
        g.NsInfoUptime_d                  = config.settings::take("/Ns/Info/Uptime/d"                  , ""s);
        g.NsInfoUptime_h                  = config.settings::take("/Ns/Info/Uptime/h"                  , ""s);
        g.NsInfoUptime_m                  = config.settings::take("/Ns/Info/Uptime/m"                  , ""s);
        g.NsInfoUptime_s                  = config.settings::take("/Ns/Info/Uptime/s"                  , ""s);

        g.NsInfoSF                        = config.settings::take("/Ns/Info/SF"                           , ""s);
        g.NsInfoSubcellSize               = config.settings::take("/Ns/Info/SF/SubcellSize"               , ""s);
        g.NsInfoLatin                     = config.settings::take("/Ns/Info/SF/Latin"                     , ""s);
        g.NsInfoCJK                       = config.settings::take("/Ns/Info/SF/CJK"                       , ""s);
        g.NsInfoThai                      = config.settings::take("/Ns/Info/SF/Thai"                      , ""s);
        g.NsInfoGeorgian                  = config.settings::take("/Ns/Info/SF/Georgian"                  , ""s);
        g.NsInfoDevanagari                = config.settings::take("/Ns/Info/SF/Devanagari"                , ""s);
        g.NsInfoArabic                    = config.settings::take("/Ns/Info/SF/Arabic"                    , ""s);
        g.NsInfoHebrew                    = config.settings::take("/Ns/Info/SF/Hebrew"                    , ""s);
        g.NsInfoEmoji                     = config.settings::take("/Ns/Info/SF/Emoji"                     , ""s);
        g.NsInfoBoxDrawing                = config.settings::take("/Ns/Info/SF/BoxDrawing"                , ""s);
        g.NsInfoLargeTypePieces           = config.settings::take("/Ns/Info/SF/LargeTypePieces"           , ""s);
        g.NsInfoStyledUnderline           = config.settings::take("/Ns/Info/SF/Style"                     , ""s);
        g.NsInfoSingleOverline            = config.settings::take("/Ns/Info/SF/Style/SingleOverline"      , ""s);
        g.NsInfoDoubleUnderline           = config.settings::take("/Ns/Info/SF/Style/DoubleUnderline"     , ""s);
        g.NsInfoSingleUnderline           = config.settings::take("/Ns/Info/SF/Style/SingleUnderline"     , ""s);
        g.NsInfoDashedUnderline           = config.settings::take("/Ns/Info/SF/Style/DashedUnderline"     , ""s);
        g.NsInfoDottedUnderline           = config.settings::take("/Ns/Info/SF/Style/DottedUnderline"     , ""s);
        g.NsInfoWavyUnderline             = config.settings::take("/Ns/Info/SF/Style/WavyUnderline"       , ""s);
        g.NsInfoWhiteSingleUnderline      = config.settings::take("/Ns/Info/SF/Style/WhiteSingleUnderline", ""s);
        g.NsInfoWhiteWavyUnderline        = config.settings::take("/Ns/Info/SF/Style/WhiteWavyUnderline"  , ""s);
        g.NsInfoRedSingleUnderline        = config.settings::take("/Ns/Info/SF/Style/RedSingleUnderline"  , ""s);
        g.NsInfoRedWavyUnderline          = config.settings::take("/Ns/Info/SF/Style/RedWavyUnderline"    , ""s);
        g.NsInfoFontStyle                 = config.settings::take("/Ns/Info/SF/FontStyle"                 , ""s);
        g.NsInfoNormal                    = config.settings::take("/Ns/Info/SF/FontStyle/Normal"          , ""s);
        g.NsInfoBlinking                  = config.settings::take("/Ns/Info/SF/FontStyle/Blinking"        , ""s);
        g.NsInfoBold                      = config.settings::take("/Ns/Info/SF/FontStyle/Bold"            , ""s);
        g.NsInfoItalic                    = config.settings::take("/Ns/Info/SF/FontStyle/Italic"          , ""s);
        g.NsInfoCharacterWidth            = config.settings::take("/Ns/Info/SF/CharacterWidth"            , ""s);
        g.NsInfoVariationSelectors        = config.settings::take("/Ns/Info/SF/VariationSelectors"        , ""s);
        g.NsInfoLongestWord               = config.settings::take("/Ns/Info/SF/LongestWord"               , ""s);
        g.NsInfoRotationFlipandMirror     = config.settings::take("/Ns/Info/SF/RotationFlipandMirror"     , ""s);
        g.NsInfoCharacterMatrix           = config.settings::take("/Ns/Info/SF/CharacterMatrix"           , ""s);
        g.NsInfoCharacterHalves           = config.settings::take("/Ns/Info/SF/CharacterHalves"           , ""s);
        g.NsInfoTuiShadows                = config.settings::take("/Ns/Info/SF/TuiShadows"                , ""s);
        g.NsInfoTuiShadowsInner           = config.settings::take("/Ns/Info/SF/TuiShadowsInner"           , ""s);
        g.NsInfoTuiShadowsOuter           = config.settings::take("/Ns/Info/SF/TuiShadowsOuter"           , ""s);
        g.NsInfosRGBBlending              = config.settings::take("/Ns/Info/SF/sRGBBlending"              , ""s);
        g.NsInfoPressCtrlCaps             = config.settings::take("/Ns/Info/SF/PressCtrlCaps"             , ""s);

        g.NsTaskbar_tooltip               = config.settings::take("/Ns/Taskbar/taskbar_tooltip"        , ""s);
        g.NsTaskbarGrips_tooltip          = config.settings::take("/Ns/Taskbar/Grips/tooltip"          , ""s);
        g.NsUserList_tooltip              = config.settings::take("/Ns/Taskbar/UserList/tooltip"       , ""s);
        g.NsAdmins_label                  = config.settings::take("/Ns/Taskbar/UserList/Admins/label"  , "admins"s);
        g.NsUsers_label                   = config.settings::take("/Ns/Taskbar/UserList/Users/label"   , "users"s);
        g.NsUser_tooltip                  = config.settings::take("/Ns/Taskbar/UserList/User/tooltip"  , ""s);
        g.NsToggle_tooltip                = config.settings::take("/Ns/Taskbar/UserList/Toggle/tooltip", ""s);
        g.NsDisconnect_label              = config.settings::take("/Ns/Taskbar/Disconnect/label"       , "Disconnect"s);
        g.NsShutdown_label                = config.settings::take("/Ns/Taskbar/Shutdown/label"         , "Shutdown"s);
        g.NsDisconnect_tooltip            = config.settings::take("/Ns/Taskbar/Disconnect/tooltip"     , ""s);
        g.NsShutdown_tooltip              = config.settings::take("/Ns/Taskbar/Shutdown/tooltip"       , ""s);

        g.NsTaskbarAppsClose_tooltip      = config.settings::take("/Ns/Taskbar/Apps/Close/tooltip"    , ""s);
        g.NsTaskbarAppsApp_tooltip        = config.settings::take("/Ns/Taskbar/Apps/App/tooltip"      , ""s);
        g.NsTaskbarApps_deftooltip        = config.settings::take("/Ns/Taskbar/Apps/deftooltip"       , ""s);
        g.NsTaskbarApps_toggletooltip     = config.settings::take("/Ns/Taskbar/Apps/toggletooltip"    , ""s);
        g.NsTaskbarApps_groupclosetooltip = config.settings::take("/Ns/Taskbar/Apps/groupclosetooltip", ""s);

        g.NsMinimizeWindow_tooltip        = config.settings::take("/Ns/MinimizeWindow/tooltip"         , ""s);
        g.NsMaximizeWindow_tooltip        = config.settings::take("/Ns/MaximizeWindow/tooltip"         , ""s);
        g.NsCloseWindow_tooltip           = config.settings::take("/Ns/CloseWindow/tooltip"            , ""s);
    }
    static void splice(xipc client)
    {
        os::dtvt::flagsz = true;
        os::dtvt::flagsz.notify_all();
        os::tty::splice(client);
    }
    static void start(text cmd, text aclass, text title = {})
    {
        //todo revise
        auto [client, server] = os::ipc::xlink();
        auto& indexer = ui::tui_domain();
        auto& config = indexer.config;
        auto ui_lock = indexer.unique_lock();
        app::shared::get_tui_config(config, ui::skin::globals());
        auto thread = std::thread{ [&, &client = client] //todo clang 15.0.0 still disallows capturing structured bindings (wait for clang 16.0.0)
        {
            app::shared::splice(client);
        }};
        auto gate_ptr = ui::gate::ctor(server, os::dtvt::vtmode);
        auto& gate = *gate_ptr;
        gate.base::resize(os::dtvt::gridsz);
        gate.base::signal(tier::general, e2::config::fps, ui::skin::globals().maxfps);
        auto appcfg = eccc{ .cmd = cmd, .title = title };
        auto applet_ptr = app::shared::builder(aclass)(appcfg, config);
        auto& applet = *applet_ptr;
        applet.base::kind(base::reflow_root);
        app::shared::applet_kb_navigation(config, applet_ptr);
        gate.attach(std::move(applet_ptr));
        gate.base::reflow(); // Fit applet_ptr to the gate size. Resize all nested objects to set base::region instead of base::socket (see dtty's ui::veer).
        ui_lock.unlock();
        gate.launch(ui_lock);
        gate.base::dequeue();
        ui_lock.lock();
        gate_ptr.reset();
        server->shut();
        client->shut();
        thread.join();
    }
}
