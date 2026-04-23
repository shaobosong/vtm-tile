// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#pragma once

#include "terminal/enums.hpp"

// terminal: Terminal UI control.
namespace netxs::ui
{
    struct term
        : public ui::form<term>
    {
        static constexpr auto classname = basename::terminal;
        enum class dragmode
        {
            none,
            word,
            line,
        };
        static constexpr auto event_source_name = std::to_array(
        {
            "keyboard",
            "mouse",
            "focus",
            "format",
            "clipboard",
            "window",
            "system",
        });

        #define proc_list \
            X(KeyEvent             ) /* */ \
            X(ExclusiveKeyboardMode) /* */ \
            X(FindNextMatch        ) /* */ \
            X(FindText             ) /* Search for explicit text (used by the find-bar UI) */ \
            X(ToggleFindBar        ) /* Show/hide/toggle the terminal find-bar overlay */ \
            X(FindBarVisible       ) /* Query find-bar visibility state */ \
            X(ScrollViewportByPage ) /* */ \
            X(ScrollViewportByCell ) /* */ \
            X(ScrollViewportToTop  ) /* */ \
            X(ScrollViewportToEnd  ) /* */ \
            X(SendKey              ) /* */ \
            X(Print                ) /* */ \
            X(PrintLn              ) /* */ \
            X(CopyViewport         ) /* */ \
            X(CopySelection        ) /* */ \
            X(PasteClipboard       ) /* */ \
            X(ClearClipboard       ) /* */ \
            X(ClipboardFormat      ) /* */ \
            X(SelectionForm        ) /* Linear/Rectangular */ \
            X(ClearSelection       ) /* */ \
            X(OneShotSelection     ) /* One-shot toggle to copy text while mouse tracking is active */ \
            X(UndoReadline         ) /* Undo for cooked read on win32 */ \
            X(RedoReadline         ) /* Redo for cooked read on win32 */ \
            X(CwdSync              ) /* */ \
            X(LineWrapMode         ) /* */ \
            X(LineAlignMode        ) /* */ \
            X(LogMode              ) /* */ \
            X(AltbufMode           ) /* */ \
            X(ForwardKeys          ) /* */ \
            X(ClearScrollback      ) /* */ \
            X(ScrollbackSize       ) /* */ \
            X(SetBackground        ) /* */ \
            X(ResetAttributes      ) /* */ \
            X(ScrollbackPadding    ) /* */ \
            X(TabLength            ) /* */ \
            X(RightToLeft          ) /* */ \
            X(EventReporting       ) /* */ \
            X(CodePage             ) /* */ \
            X(Restart              ) /* */ \
            X(Quit                 ) /* */ \

        struct methods
        {
            #define X(_proc) static constexpr auto _proc = #_proc;
            proc_list
            #undef X
        };

        #undef proc_list

        struct commands
        {
            struct erase
            {
                struct line
                {
                    enum : si32
                    {
                        right = 0,
                        left  = 1,
                        all   = 2,
                        wraps = 3,
                    };
                };
                struct display
                {
                    enum : si32
                    {
                        below      = 0,
                        above      = 1,
                        viewport   = 2,
                        scrollback = 3,
                    };
                };
            };
            struct ui
            {
                enum commands : si32
                {
                    center,
                    toggleraw,
                    togglewrp,
                    togglejet,
                    togglesel,
                    toggleselalt,
                    restart,
                    sighup,
                    undo,
                    redo,
                    deselect,
                };
            };
            struct cursor // See pro::caret.
            {
                enum : si32
                {
                    def_style          = 0, // blinking box
                    blinking_box       = 1, // blinking box (default)
                    steady_box         = 2, // steady box
                    blinking_underline = 3, // blinking underline
                    steady_underline   = 4, // steady underline
                    blinking_I_bar     = 5, // blinking I-bar
                    steady_I_bar       = 6, // steady I-bar
                };
            };
            struct atexit
            {
                enum codes : si32
                {
                    ask,     // Stay open.
                    smart,   // Stay open if exit code != 0.
                    close,   // Always quit.
                    restart, // Restart session.
                    retry,   // Restart session if exit code != 0.
                };
            };
            struct fx
            {
                enum shader : si32
                {
                    xlight,
                    color,
                    invert,
                    reverse,
                };
            };
        };

        #include "terminal/config.hpp"
        #include "terminal/state.hpp"
        #include "terminal/trackers/mouse.hpp"
        #include "terminal/trackers/focus.hpp"
        #include "terminal/trackers/window.hpp"
        #include "terminal/trackers/palette.hpp"
        #include "terminal/bufferbase.hpp"
        #include "terminal/alt_screen.hpp"
        #include "terminal/scroll_buf.hpp"
        #include "terminal/term_body.hpp"
    };

    #include "terminal/dtvt.hpp"
}
