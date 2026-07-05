// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/settings_dialog.hpp: the Edit -> Settings dialog — a FileZilla-style,
// top-tabbed settings form (Connection | SFTP) ported from FileZilla's Connection
// and Connection/SFTP option pages (TLS excluded). Rendered in the command_bar
// style (direct canvas paint + manual keyboard/mouse handling) as a centered modal
// card inside a dimming full-window overlay, mirroring show_close_confirmation.
//
// Reuse: input fields and buttons are painted by the shared paint_field / paint_button
// helpers (panes.hpp) so they are identical to the Quick Connect bar's; the private-key
// file picker instantiates the Local Site browser (make_file_pane); the private-key
// table mirrors the queue Transferring table.

#include "panes.hpp"     // theme, put_str, paint_field, paint_button, edit_*, make_file_pane, sd_hit
#include "connectbar.hpp" // connect-bar field/secret editor conventions
#include "prompts.hpp"   // make_secret_dialog (passphrase / save-path modal for key conversion)

#if !defined(_WIN32)
    #include <sys/wait.h> // waitpid for the pvputtygen one-shot client.
#endif

namespace netxs::app::parvion
{
    namespace sd
    {
        enum tab_t { tab_connection, tab_sftp, tab_count };
        // Editable numeric fields (both tabs). Each carries its text plus the connect-bar
        // field editor state (caret/scroll) and its painted box for hit-testing.
        enum field_t { f_timeout, f_retries, f_delay, f_threshold, f_maxconn, f_count };

        struct field
        {
            text  val;       // The text being edited (digits only).
            si32  caret = 0; // Grapheme-cluster caret index.
            si32  off   = 0; // Horizontal scroll offset (cells).
            rect  box{};     // Cached input box (render -> mouse).
            si32  lo = 0;    // Clamp range (inclusive)...
            si32  hi = 0;    // ...applied on commit.
            si32  tab = 0;   // Which tab hosts this field.
        };

        // Hit-box kinds the mouse layer recognizes, cached each render.
        struct hitboxes
        {
            std::array<rect, tab_count> tab_box{};   // Top tab buttons.
            rect compression{};                       // "Enable compression" checkbox row.
            rect unit{};                              // Threshold unit dropdown.
            rect hash_algo{};                         // Hash-on-transfer algorithm dropdown ("None" = disabled).
            rect addkey{}, removekey{};               // Key management buttons.
            rect ok{}, cancel{};                      // Dialog buttons.
        };

        // Default content width inside a group box (matches FileZilla's roomy layout).
        inline constexpr auto pad_x = si32{ 2 };  // Card left/right inner padding.

        // --- Private-key table (strictly matches the Local Site browser table, panes.hpp) -----
        // A self-contained 3-column table (Filename / Comment / Data) with resizable columns,
        // advanced H/V scrollbars (thumb/track + click + wheel) and single/ctrl/shift/drag
        // multi-selection — the same behaviors as the file-browser pane, modeled on pane_state.
        inline constexpr auto kt_ncol    = si32{ 3 };
        inline constexpr auto kt_col_min = si32{ 2 };
        inline constexpr auto kt_col_max = si32{ 200 };
        inline const     auto kt_headers = std::array<view, kt_ncol>{ "Filename", "Comment", "Data" };

        struct keytbl_state
        {
            std::array<si32, kt_ncol> col_w{ 18, 18, 28 }; // Resizable column widths.
            std::set<si32>     marked{};        // Multi-selection (highlighted rows).
            si32               sel = -1;        // Cursor / shift anchor target.
            si32               sel_anchor = 0;  // Shift-range anchor.
            si32               scroll = 0;      // First visible body row.
            si32               hscroll = 0;     // Horizontal cell offset.
            si32               hover_border = -1; // Column border under the cursor (-1 = none).
            si32               col_drag = -1;   // Column border being width-dragged.
            bool               vsb_hover = faux, vsb_drag = faux; si32 vsb_grab = 0;
            bool               hsb_hover = faux, hsb_drag = faux; si32 hsb_grab = 0;
            bool               rubber = faux;   // Live left-drag rubber-band selection.
            si32               rubber_a = -1, rubber_b = -1;
            std::set<si32>     drag_base{};     // Selection snapshot for ctrl+drag merge.
            bool               rubber_ctrl = faux, rubber_add = true;
            rect               area{};          // Table rect (card-local origin + size), set each render.
            si32               rows = 0, disp_w = 0, content_w = 0, hsb_y = 0, div_bottom = 1;
            bool               has_vsb = faux, has_hsb = faux;
            si32               drag_mode = 0;   // Active drag: 0 none, 1 col, 2 vsb, 3 hsb, 4 rubber.
        };
    }

    struct settings_state
    {
        sftp_remote*       ctrl = nullptr;
        parvion_settings   draft;            // Edited copy; committed to ctrl on OK.
        si32               tab  = sd::tab_connection;
        std::array<sd::field, sd::f_count> fields{};
        si32               active = -1;       // Field receiving input (-1 = none).
        bool               compression = faux;
        si32               threshold_unit = 2;
        bool               hash_on_transfer = faux; // "Calculate target file hash during transfers".
        si32               hash_algo = 2;           // Algorithm index; see settings.hpp helpers.
        sd::keytbl_state   kt;                // Private-key table (file-browser-style; selection/scroll/columns).
        // Parsed key metadata, parallel to draft.keyfiles (filled by pvputtygen).
        std::vector<text>  key_comment;
        std::vector<text>  key_data;
        rect               card{};            // Cached card rect within the overlay (render -> mouse).
        sd::hitboxes       hit{};
        bool               hover_ok = faux, hover_cancel = faux, hover_add = faux, hover_remove = faux, hover_unit = faux, hover_hashalgo = faux;
        bool               press_ok = faux, press_cancel = faux, press_add = faux, press_remove = faux;
        bool               focused = faux;
        si32               drag_field = -1;   // Field whose caret a left-drag is scrubbing.
        netxs::wptr<ui::base> overlay_wp;     // The overlay cake (to close on OK/Cancel).
        netxs::wptr<ui::base> window_wp;      // App window (anchor for the file picker overlay).
        netxs::wptr<ui::base> card_wp;        // The card widget (to deface after picker actions).

        // Seed the draft + editable fields from the controller's persisted settings.
        void seed()
        {
            if (ctrl) draft = ctrl->cfg;
            compression      = draft.compression;
            threshold_unit   = draft.threshold_unit;
            hash_on_transfer = draft.hash_on_transfer;
            hash_algo        = draft.hash_algo;
            auto setf = [&](sd::field_t i, si32 v, si32 lo, si32 hi, si32 tab)
            {
                auto& f = fields[i];
                f.val = std::to_string(v);
                f.caret = cluster_count(f.val);
                f.off = 0; f.lo = lo; f.hi = hi; f.tab = tab;
            };
            setf(sd::f_timeout,   draft.timeout,         0, 9999, sd::tab_connection);
            setf(sd::f_retries,   draft.reconnect_count, 0,   99, sd::tab_connection);
            setf(sd::f_delay,     draft.reconnect_delay, 0,  999, sd::tab_connection);
            setf(sd::f_threshold, draft.threshold_value, 1, 1024 * 1024, sd::tab_sftp);
            setf(sd::f_maxconn,   draft.max_connections, 1,   10, sd::tab_sftp);
            key_comment.assign(draft.keyfiles.size(), text{});
            key_data.assign(draft.keyfiles.size(), text{});
        }

        // Read a field's text back as a clamped integer.
        auto field_int(sd::field_t i) const -> si32
        {
            auto& f = fields[i];
            auto n = f.val.empty() ? 0 : std::atoi(f.val.c_str());
            return std::clamp(n, f.lo, f.hi);
        }
        // Gather the edited fields into the draft and clamp.
        void harvest()
        {
            draft.timeout         = field_int(sd::f_timeout);
            draft.reconnect_count = field_int(sd::f_retries);
            draft.reconnect_delay = field_int(sd::f_delay);
            draft.threshold_value = field_int(sd::f_threshold);
            draft.max_connections = field_int(sd::f_maxconn);
            draft.threshold_unit     = threshold_unit;
            draft.compression        = compression;
            draft.hash_on_transfer   = hash_on_transfer;
            draft.hash_algo          = hash_algo;
            draft.clamp();
        }
    };

    // --- pvputtygen client: classify / convert a private key, parse its Comment + Data ----------
    // Speaks cmdgen.c's line protocol (file/encrypted/password/write/fingerprint/comment) over a
    // `vtm-tile -r pvputtygen` one-shot, mirroring FileZilla's CFZPuttyGenInterface. Each call is a
    // self-contained batch (the child exits on the trailing blank line). Synchronous — fast enough
    // on the UI thread for a single key; the convert path also feeds a passphrase + a write target.
    // Run pvputtygen once over `script`, collecting the fzprintf reply ('0') payload lines in order
    // into `replies`. Returns faux if the helper couldn't be launched or any command answered
    // sftpError ('2') (e.g. wrong passphrase). Cross-platform: fork/exec on POSIX, CreateProcessW on
    // Windows (the multi-call self, `-r pvputtygen`); both speak cmdgen.c's line protocol over pipes
    // (write the whole script, close stdin -> EOF, read all stdout). Payloads are small, so the
    // write-then-read order can't deadlock the pipes.
    inline auto pvputtygen_run(text const& script, std::vector<text>& replies) -> bool
    {
        replies.clear();
        auto out = text{};
    #if !defined(_WIN32)
        auto exe = os::process::binary(); // Resolved in the PARENT: this call allocates, and pvputtygen_run
                                          // now runs on a background thread — doing it post-fork in the child
                                          // could deadlock on the malloc lock. c_str() below is alloc-free.
        auto inpipe = std::array<int, 2>{};
        auto outpipe = std::array<int, 2>{};
        if (::pipe(inpipe.data()) != 0 || ::pipe(outpipe.data()) != 0) return faux;
        auto pid = ::fork();
        if (pid < 0) return faux;
        if (pid == 0)
        {
            ::dup2(inpipe[0], 0);
            ::dup2(outpipe[1], 1);
            ::close(inpipe[0]); ::close(inpipe[1]); ::close(outpipe[0]); ::close(outpipe[1]);
            ::execl(exe.c_str(), exe.c_str(), "-r", "pvputtygen", (char*)nullptr);
            ::_exit(127);
        }
        ::close(inpipe[0]); ::close(outpipe[1]);
        auto wr = ::write(inpipe[1], script.data(), script.size()); (void)wr;
        ::close(inpipe[1]);
        auto buf = std::array<char, 4096>{};
        for (auto n = ssize_t{}; (n = ::read(outpipe[0], buf.data(), buf.size())) > 0; ) out.append(buf.data(), (size_t)n);
        ::close(outpipe[0]);
        auto status = 0;
        ::waitpid(pid, &status, 0);
    #else
        // Inheritable pipes: child stdin = inRd, child stdout = outWr; our ends stay private.
        auto sa = SECURITY_ATTRIBUTES{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        auto inRd = HANDLE{}, inWr = HANDLE{}, outRd = HANDLE{}, outWr = HANDLE{};
        if (!::CreatePipe(&inRd, &inWr, &sa, 0)) return faux;
        if (!::CreatePipe(&outRd, &outWr, &sa, 0)) { ::CloseHandle(inRd); ::CloseHandle(inWr); return faux; }
        ::SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
        auto nul = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
        auto si = STARTUPINFOEXW{ sizeof(STARTUPINFOEXW) };
        si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput  = inRd;
        si.StartupInfo.hStdOutput = outWr;
        si.StartupInfo.hStdError  = nul;
        HANDLE inherit[] = { inRd, outWr, nul }; // Inherit ONLY these (Win32 equivalent of closefrom).
        auto attrbuf  = std::vector<byte>{};
        auto attrsize = SIZE_T{ 0 };
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrsize);
        attrbuf.resize(attrsize);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrbuf.data());
        auto procsinf = PROCESS_INFORMATION{}; // (not `pi` — that shadows a global on MSVC: C4459).
        auto cmd  = "\"" + os::process::binary() + "\" -r pvputtygen";
        auto wcmd = utf::to_utf(cmd);
        auto ok = ::InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrsize)
               && ::UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)
               && ::CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &procsinf);
        if (si.lpAttributeList) ::DeleteProcThreadAttributeList(si.lpAttributeList);
        ::CloseHandle(inRd); ::CloseHandle(outWr); if (nul) ::CloseHandle(nul);
        if (!ok) { ::CloseHandle(inWr); ::CloseHandle(outRd); return faux; }
        ::CloseHandle(procsinf.hThread);
        for (auto done = DWORD{ 0 }, left = (DWORD)script.size(); left; )
        {
            auto wrote = DWORD{ 0 };
            if (!::WriteFile(inWr, script.data() + done, left, &wrote, nullptr) || !wrote) break;
            done += wrote; left -= wrote;
        }
        ::CloseHandle(inWr); // EOF -> the helper exits its read loop.
        auto buf = std::array<char, 4096>{};
        for (auto n = DWORD{ 0 }; ::ReadFile(outRd, buf.data(), (DWORD)buf.size(), &n, nullptr) && n; ) out.append(buf.data(), n);
        ::CloseHandle(outRd);
        ::WaitForSingleObject(procsinf.hProcess, INFINITE);
        ::CloseHandle(procsinf.hProcess);
    #endif
        // Banner lines start with a letter and are ignored; '0' = sftpReply payload, '2' = sftpError.
        auto err = faux;
        for (auto pos = size_t{}; pos < out.size(); )
        {
            auto eol  = out.find('\n', pos);
            auto line = out.substr(pos, eol == text::npos ? text::npos : eol - pos);
            pos = eol == text::npos ? out.size() : eol + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back(); // Windows text-mode stdout adds '\r'.
            if (line.empty()) continue;
            if      (line[0] == '0') replies.push_back(line.substr(1));
            else if (line[0] == '2') err = true;
        }
        return !err;
    }
    // Parse a usable (already-ppk or unencrypted) key's Comment + Data (fingerprint). Used by the
    // reopen-reparse loop; keeps the historical "ok"/"convertible" success semantics intact.
    inline auto sd_keyinfo(text const& keypath, text& comment, text& data) -> bool
    {
        auto replies = std::vector<text>{};
        auto ok = pvputtygen_run("file " + keypath + "\nfingerprint\ncomment\n\n", replies);
        data    = replies.size() > 1 ? replies[1] : text{};
        comment = replies.size() > 2 ? replies[2] : text{};
        auto okresult = !replies.empty() && (replies[0] == "ok" || replies[0] == "convertible");
        return okresult && ok && replies.size() >= 2;
    }

    // --- group box + word-wrap helpers ----------------------------------------------
    // Draw a FileZilla-style titled group box border into `r` (card-local). Title sits in
    // the top edge: "┌─ Title ───┐". Interior is left for the caller to fill with rows.
    inline void sd_box(auto& canvas, rect r, view title)
    {
        if (r.size.x < 2 || r.size.y < 2) return;
        auto x0 = r.coor.x, y0 = r.coor.y, x1 = r.coor.x + r.size.x - 1, y1 = r.coor.y + r.size.y - 1;
        auto bc = ui32{ theme::subtext };
        auto putc = [&](si32 x, si32 y, view g){ put_str(canvas, x, y, g, bc, theme::bg, 2); };
        putc(x0, y0, "┌"); putc(x1, y0, "┐"); putc(x0, y1, "└"); putc(x1, y1, "┘");
        for (auto x = x0 + 1; x < x1; ++x) { putc(x, y0, "─"); putc(x, y1, "─"); }
        for (auto y = y0 + 1; y < y1; ++y) { putc(x0, y, "│"); putc(x1, y, "│"); }
        // Title: "─ Title ─" starting two cells in.
        auto cap = text{ "─ " } + text{ title } + " ";
        put_str(canvas, x0 + 1, y0, cap, theme::text_fg, theme::bg, std::max(0, r.size.x - 3));
    }
    // Greedy word-wrap into lines of at most `maxw` display cells (auto-wrap help text).
    inline auto sd_wrap(view s, si32 maxw) -> std::vector<text>
    {
        auto lines = std::vector<text>{};
        if (maxw <= 0) return lines;
        auto cur = text{};
        auto flush = [&]{ lines.push_back(cur); cur.clear(); };
        auto words = std::vector<text>{};
        auto w = text{};
        for (auto c : s) { if (c == ' ') { if (!w.empty()) { words.push_back(w); w.clear(); } } else w += c; }
        if (!w.empty()) words.push_back(w);
        for (auto& word : words)
        {
            auto cand = cur.empty() ? word : cur + " " + word;
            if (cell_width(cand) <= maxw) cur = cand;
            else { if (!cur.empty()) flush(); cur = word; }
        }
        if (!cur.empty() || lines.empty()) flush();
        return lines;
    }

    namespace sd
    {
        inline constexpr auto field_w = std::array<si32, f_count>{ 6, 6, 6, 6, 6 }; // All input fields are 6 cells wide.

        // --- Form text, defined once so the renderer and the width negotiator stay in sync ----
        inline constexpr auto lbl_timeout   = view{ "Timeout in seconds:" };
        inline constexpr auto hnt_timeout   = view{ "(10-9999, 0 to disable)" };
        inline constexpr auto help_timeout  = view{ "If no data is sent or received during an operation for longer than the specified time, the connection will be closed and Parvion will try to reconnect." };
        inline constexpr auto lbl_retries   = view{ "Maximum number of retries:" };
        inline constexpr auto hnt_retries   = view{ "(0-99, 0 for unlimited)" };
        inline constexpr auto lbl_delay     = view{ "Delay between failed login attempts:" };
        inline constexpr auto hnt_delay     = view{ "(0-999 seconds)" };
        inline constexpr auto help_reconn   = view{ "Please note that some servers might ban you if you try to reconnect too often or in too short intervals." };
        inline constexpr auto help_pubkey   = view{ "To support public key authentication, Parvion needs to know the private keys to use." };
        inline constexpr auto lbl_privkeys  = view{ "Private keys:" };
        inline constexpr auto btn_addkey    = view{ " Add key file... " };
        inline constexpr auto btn_removekey = view{ " Remove key " };
        inline constexpr auto chk_compress  = view{ "\xE2\x96\xA1 Enable compression" };

        inline constexpr auto lbl_threshold = view{ "Enable parallel transfers for files larger than:" };
        inline constexpr auto lbl_maxconn   = view{ "Maximum parallel connections for a single file:" };
        inline constexpr auto hnt_maxconn   = view{ "(1-10)" };
        // Hash verification group (single dropdown; "None" disables transfer hashing).
        inline constexpr auto lbl_hash_xfer = view{ "Calculate target file hash during transfers:" };
        inline constexpr auto hash_none     = view{ "None" };
        inline constexpr auto unit_widest   = view{ " Byte \xE2\x96\xBE " }; // The widest unit-dropdown caption.
        inline constexpr auto kt_min_w      = si32{ 24 }; // Key table negotiates a small floor (it scrolls horizontally).
    }

    // --- Minimum-width negotiation: internal components -> group box -> dialog -------
    // Each internal component reports the minimum group-box CONTENT width it needs;
    // sd_box_dialog_w turns that into the card width the box requires (box border + one
    // pad cell per side, plus the card's left/right margins). The dialog's minimum width
    // is the max across every box (and the tab strip / button row). New components join the
    // negotiation simply by adding their own sd_box_dialog_w(...) line in sd_min_width.
    inline auto sd_box_dialog_w(si32 content_w) -> si32 { return content_w + 4 + 2 * sd::pad_x; }
    // A vertically-aligned field group's min content width: the longest label, a gap, the
    // shared 6-cell field, a 2-cell gap, then the longest trailing text (hint or control).
    inline auto sd_fieldgroup_w(std::initializer_list<std::pair<view, view>> rows) -> si32
    {
        auto lbl = si32{ 0 }, trail = si32{ 0 };
        for (auto const& r : rows) { lbl = std::max(lbl, (si32)cell_width(r.first)); trail = std::max(trail, (si32)cell_width(r.second)); }
        return lbl + 1 + sd::field_w[0] + 2 + trail;
    }
    inline auto sd_min_width() -> si32
    {
        auto w = si32{ 0 };
        // Connection tab group boxes (help text wraps, so it doesn't drive the width).
        w = std::max(w, sd_box_dialog_w(sd_fieldgroup_w({ { sd::lbl_timeout, sd::hnt_timeout } })));
        w = std::max(w, sd_box_dialog_w(sd_fieldgroup_w({ { sd::lbl_retries, sd::hnt_retries },
                                                          { sd::lbl_delay,   sd::hnt_delay } })));
        // SFTP tab group boxes.
        w = std::max(w, sd_box_dialog_w(sd_fieldgroup_w({ { sd::lbl_threshold, sd::unit_widest },
                                                          { sd::lbl_maxconn,   sd::hnt_maxconn } })));
        w = std::max(w, sd_box_dialog_w((si32)cell_width(sd::btn_addkey) + 1 + (si32)cell_width(sd::btn_removekey)));
        w = std::max(w, sd_box_dialog_w((si32)cell_width(sd::chk_compress)));
        w = std::max(w, sd_box_dialog_w((si32)cell_width(sd::lbl_hash_xfer) + 1 + 12)); // label + algorithm dropdown.
        w = std::max(w, sd_box_dialog_w(sd::kt_min_w));
        // Tab strip and the right-aligned OK/Cancel block.
        w = std::max(w, (si32)cell_width("Connection") + 2 + (si32)cell_width("SFTP") + 2 + 2 * sd::pad_x);
        w = std::max(w, 4 + 1 + 8 + 2 + 2 * sd::pad_x);
        return w;
    }

    // --- Private-key table helpers (mirror panes.hpp's file-browser table) ----------
    // The table is painted inside the settings card at kt.area.coor (card-local), in
    // table-local coords: row 0 = column header, rows 1.. = body, hsb at hsb_y, vsb at
    // the right column. Geometry helpers mirror p_col_x / pane_layout / pane_scrollbar.
    inline auto kt_col_x(sd::keytbl_state const& kt, si32 i) -> si32
    {
        auto x = si32{ 0 };
        for (auto j = si32{}; j < i; ++j) x += kt.col_w[(size_t)j];
        return x;
    }
    inline auto kt_border_cx(sd::keytbl_state const& kt, si32 i) -> si32 { return kt_col_x(kt, i) + kt.col_w[(size_t)i] - 1; }
    inline auto kt_content_w(sd::keytbl_state const& kt) -> si32
    {
        auto x = si32{ 0 };
        for (auto i = si32{}; i < sd::kt_ncol; ++i) x += kt.col_w[(size_t)i];
        return x;
    }
    inline auto kt_row_w(sd::keytbl_state const& kt) -> si32 { return std::clamp(kt.content_w - kt.hscroll, 0, kt.disp_w); }
    // Two-pass VSB<->HSB settling (mirrors pane_layout); body has one header row above it.
    inline void kt_layout(sd::keytbl_state& kt, si32 w, si32 h, si32 content_w, si32 n)
    {
        kt.content_w = content_w;
        auto avail = std::max(0, h - 1);
        for (auto pass = si32{}; pass < 2; ++pass)
        {
            kt.has_vsb = avail > 0 && n > avail;
            kt.disp_w  = w - (kt.has_vsb ? 1 : 0);
            kt.has_hsb = content_w > kt.disp_w;
            kt.rows    = std::max(0, avail - (kt.has_hsb ? 1 : 0));
            kt.has_vsb = kt.rows > 0 && n > kt.rows;
            kt.disp_w  = w - (kt.has_vsb ? 1 : 0);
            kt.has_hsb = content_w > kt.disp_w;
            kt.rows    = std::max(0, avail - (kt.has_hsb ? 1 : 0));
        }
        kt.hsb_y = 1 + kt.rows;
    }
    struct kt_sb { bool ok = faux; si32 x = 0, top = 0, track_h = 0, thumb_y = 0, thumb_h = 0, maxscroll = 0; };
    inline auto kt_vsb(sd::keytbl_state const& kt, si32 n) -> kt_sb
    {
        auto sb = kt_sb{};
        sb.ok = kt.has_vsb && kt.rows > 0 && n > kt.rows;
        if (!sb.ok) return sb;
        sb.x = kt.area.size.x - 1; sb.top = 1; sb.track_h = kt.rows;
        sb.thumb_h = std::max(1, kt.rows * kt.rows / n);
        sb.maxscroll = n - kt.rows;
        sb.thumb_y = sb.top + (kt.rows - sb.thumb_h) * kt.scroll / sb.maxscroll;
        return sb;
    }
    inline auto kt_hsb(sd::keytbl_state const& kt) -> kt_sb
    {
        auto sb = kt_sb{};
        sb.ok = kt.has_hsb && kt.disp_w > 0 && kt.content_w > kt.disp_w;
        if (!sb.ok) return sb;
        sb.x = 0; sb.top = kt.hsb_y; sb.track_h = kt.disp_w;
        sb.thumb_h = std::max(1, kt.disp_w * kt.disp_w / kt.content_w);
        sb.maxscroll = kt.content_w - kt.disp_w;
        sb.thumb_y = (kt.disp_w - sb.thumb_h) * kt.hscroll / sb.maxscroll;
        return sb;
    }
    inline void kt_vsb_scroll_to(sd::keytbl_state& kt, si32 ly, kt_sb const& sb)
    {
        auto travel = sb.track_h - sb.thumb_h;
        if (travel <= 0) return;
        kt.scroll = std::clamp((ly - kt.vsb_grab - sb.top) * sb.maxscroll / travel, 0, sb.maxscroll);
    }
    inline void kt_hsb_scroll_to(sd::keytbl_state& kt, si32 lx, kt_sb const& sb)
    {
        auto travel = sb.track_h - sb.thumb_h;
        if (travel <= 0) return;
        kt.hscroll = std::clamp((lx - kt.hsb_grab - sb.x) * sb.maxscroll / travel, 0, sb.maxscroll);
    }
    // Row index for a table-local row `ly` (body rows start at 1); -1 outside the body.
    inline auto kt_hit_row(sd::keytbl_state const& kt, si32 ly) -> si32
    {
        auto vis = ly - 1;
        if (vis < 0 || vis >= kt.rows) return -1;
        return kt.scroll + vis;
    }
    // Cell text for the key table: col 0 = filename (basename), 1 = comment, 2 = data.
    inline auto kt_cell(settings_state const& st, si32 col, si32 row) -> text
    {
        if (row < 0 || row >= (si32)st.draft.keyfiles.size()) return {};
        if (col == 0) return fs::path{ st.draft.keyfiles[(size_t)row] }.filename().string();
        if (col == 1) return row < (si32)st.key_comment.size() ? st.key_comment[(size_t)row] : text{};
        return row < (si32)st.key_data.size() ? st.key_data[(size_t)row] : text{};
    }
    // Paint the key table into the card canvas at kt.area (card-local). Mirrors pane_render.
    inline void kt_render(auto& canvas, settings_state& st, bool focused)
    {
        auto& kt = st.kt;
        auto ox = kt.area.coor.x, oy = kt.area.coor.y;
        auto w  = kt.area.size.x, h = kt.area.size.y;
        if (w <= 0 || h <= 0) return;
        auto n = (si32)st.draft.keyfiles.size();
        auto content_w = kt_content_w(kt);
        kt_layout(kt, w, h, content_w, n);
        kt.sel     = std::clamp(kt.sel, -1, std::max(-1, n - 1));
        kt.scroll  = std::clamp(kt.scroll, 0, std::max(0, n - kt.rows));
        kt.hscroll = std::clamp(kt.hscroll, 0, std::max(0, kt.content_w - kt.disp_w));
        auto hs = kt.hscroll;
        auto clipw = kt.disp_w;
        auto cx = std::array<si32, sd::kt_ncol>{};
        for (auto i = si32{}; i < sd::kt_ncol; ++i) cx[(size_t)i] = kt_col_x(kt, i);
        // Paint one column's text, ellipsized + horizontally scrolled + clipped to [0,clipw).
        auto col = [&](si32 content_x, si32 colw, si32 ry, view s, ui32 fg, ui32 bg)
        {
            auto t  = fit_ellipsis(s, colw);
            auto v  = view{ t };
            auto sx = content_x - hs;
            if (sx >= clipw || sx + colw <= 0) return;
            if (sx < 0)
            {
                auto cl  = cell_to_cluster(v, -sx);
                auto cut = caret_cell(v, cl);
                v = v.substr(cluster_to_byte(v, cl)); colw -= cut; sx += cut;
            }
            auto room = std::min(colw, clipw - sx);
            if (room > 0 && sx >= 0 && sx < clipw) put_str(canvas, ox + sx, oy + ry, v, fg, bg, room);
        };
        auto draw_dividers = [&](si32 ry, ui32 bg)
        {
            for (auto i = si32{}; i < sd::kt_ncol; ++i)
            {
                auto bx = kt_border_cx(kt, i) - hs;
                if (bx < 0 || bx >= clipw) continue;
                auto hot = kt.hover_border == i || kt.col_drag == i;
                put_str(canvas, ox + bx, oy + ry, "\xE2\x94\x82", hot ? ui32{ theme::text_fg } : ui32{ theme::subtext }, bg, 1);
            }
        };
        // Header (row 0).
        canvas.fill(rect{{ ox, oy }, { w, 1 }}, [&](cell& c){ c.bgc(theme::surface); });
        for (auto i = si32{}; i < sd::kt_ncol; ++i) col(cx[(size_t)i], kt.col_w[(size_t)i] - 1, 0, sd::kt_headers[(size_t)i], theme::subtext, theme::surface);
        draw_dividers(0, theme::surface);
        // Body.
        auto rows_drawn = si32{};
        for (auto vis = si32{}; vis < kt.rows; ++vis)
        {
            auto row = kt.scroll + vis;
            if (row >= n) break;
            ++rows_drawn;
            auto ry = 1 + vis;
            auto is_sel = kt.marked.count(row) != 0;
            auto rbg = is_sel ? ui32{ theme::sel_bg } : ui32{ theme::bg };
            if (is_sel)
            {
                canvas.fill(rect{{ ox, oy + ry }, { kt_row_w(kt), 1 }}, [&](cell& c){ c.bgc(theme::sel_bg); });
                if (focused) canvas.fill(rect{{ ox, oy + ry }, { 1, 1 }}, [&](cell& c){ c.bgc(theme::sel_bg_act); });
            }
            for (auto i = si32{}; i < sd::kt_ncol; ++i) col(cx[(size_t)i], kt.col_w[(size_t)i] - 1, ry, kt_cell(st, i, row), theme::text_fg, rbg);
            draw_dividers(ry, rbg);
        }
        kt.div_bottom = 1 + rows_drawn;
        // Scrollbars (same half-block design as the file pane).
        if (auto sb = kt_vsb(kt, n); sb.ok)
        {
            auto mark = (kt.vsb_drag || kt.vsb_hover) ? "\xe2\x96\x88" : "\xe2\x96\x90"; // █ : ▐
            canvas.fill(rect{{ ox + sb.x, oy + sb.top }, { 1, sb.track_h }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::sb_track).txt(mark); });
            auto tc = kt.vsb_drag ? ui32{ theme::sb_drag } : kt.vsb_hover ? ui32{ theme::sb_hover } : ui32{ theme::sb_thumb };
            canvas.fill(rect{{ ox + sb.x, oy + sb.thumb_y }, { 1, sb.thumb_h }}, [&](cell& c){ c.bgc(theme::bg).fgc(tc).txt(mark); });
        }
        if (auto sb = kt_hsb(kt); sb.ok)
        {
            auto mark = (kt.hsb_drag || kt.hsb_hover) ? "\xe2\x96\x84" : "\xe2\x96\x82"; // ▄ : ▂
            canvas.fill(rect{{ ox + sb.x, oy + sb.top }, { sb.track_h, 1 }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::sb_track).txt(mark); });
            auto tc = kt.hsb_drag ? ui32{ theme::sb_drag } : kt.hsb_hover ? ui32{ theme::sb_hover } : ui32{ theme::sb_thumb };
            canvas.fill(rect{{ ox + sb.x + sb.thumb_y, oy + sb.top }, { sb.thumb_h, 1 }}, [&](cell& c){ c.bgc(theme::bg).fgc(tc).txt(mark); });
        }
    }

    // Widest content + header for column `col` (drives the double-click border auto-fit).
    inline auto kt_col_content_w(settings_state const& st, si32 col) -> si32
    {
        auto w = cell_width(sd::kt_headers[(size_t)col]);
        for (auto row = si32{}; row < (si32)st.draft.keyfiles.size(); ++row) w = std::max(w, cell_width(kt_cell(st, col, row)));
        return w;
    }
    inline auto kt_in_area(settings_state const& st, si32 mx, si32 my) -> bool
    {
        auto& a = st.kt.area;
        return a.size.x > 0 && mx >= a.coor.x && mx < a.coor.x + a.size.x && my >= a.coor.y && my < a.coor.y + a.size.y;
    }
    // Press selection (table-local lx,ly): mirrors the file pane's LeftDown body logic. Presses on a
    // scrollbar or a column border are deferred to the drag/click handlers (returns false there).
    inline auto kt_on_down(settings_state& st, si32 lx, si32 ly, bool ctl, bool shft) -> bool
    {
        auto& kt = st.kt;
        auto n = (si32)st.draft.keyfiles.size();
        if (auto sb = kt_vsb(kt, n); sb.ok && lx == sb.x && ly >= sb.top && ly < sb.top + sb.track_h) return faux;
        if (auto sb = kt_hsb(kt);    sb.ok && ly == sb.top && lx >= sb.x && lx < sb.x + sb.track_h) return faux;
        if (ly >= 0 && ly < kt.div_bottom)
            for (auto i = si32{}; i < sd::kt_ncol; ++i) if (lx == kt_border_cx(kt, i) - kt.hscroll) return faux;
        kt.drag_base = kt.marked; // Snapshot for a possible ctrl+drag merge.
        auto row = (lx < kt_row_w(kt)) ? kt_hit_row(kt, ly) : -1;
        if (row >= 0 && row < n)
        {
            if (shft)
            {
                auto lo = std::min(kt.sel_anchor, row), hi = std::max(kt.sel_anchor, row);
                kt.marked.clear();
                for (auto j = lo; j <= hi; ++j) kt.marked.insert(j);
                kt.sel = row;
            }
            else if (ctl)
            {
                if (kt.marked.count(row)) kt.marked.erase(row); else kt.marked.insert(row);
                kt.sel = row; kt.sel_anchor = row;
            }
            else { kt.marked = { row }; kt.sel = row; kt.sel_anchor = row; }
            return true;
        }
        if (!ctl && ly >= 1 && ly < 1 + kt.rows && !kt.marked.empty()) { kt.marked.clear(); kt.sel = -1; return true; } // Blank press clears.
        return faux;
    }
    // Click on a scrollbar rail outside the thumb: page toward the click (table-local lx,ly).
    inline auto kt_on_click(settings_state& st, si32 lx, si32 ly) -> bool
    {
        auto& kt = st.kt;
        auto n = (si32)st.draft.keyfiles.size();
        if (auto sb = kt_vsb(kt, n); sb.ok && lx == sb.x && ly >= sb.top && ly < sb.top + sb.track_h)
        {
            if      (ly < sb.thumb_y)               kt.scroll = std::max(0, kt.scroll - kt.rows);
            else if (ly >= sb.thumb_y + sb.thumb_h) kt.scroll = std::min(sb.maxscroll, kt.scroll + kt.rows);
            return true;
        }
        if (auto sb = kt_hsb(kt); sb.ok && ly == sb.top && lx >= sb.x && lx < sb.x + sb.track_h)
        {
            auto tx = sb.x + sb.thumb_y;
            if      (lx < tx)               kt.hscroll = std::max(0, kt.hscroll - kt.disp_w);
            else if (lx >= tx + sb.thumb_h) kt.hscroll = std::min(sb.maxscroll, kt.hscroll + kt.disp_w);
            return true;
        }
        return faux;
    }
    // Double-click a column border: auto-fit it to its widest content + header (table-local lx,ly).
    inline auto kt_on_dclick(settings_state& st, si32 lx, si32 ly) -> bool
    {
        auto& kt = st.kt;
        if (ly >= 0 && ly < kt.div_bottom)
            for (auto i = si32{}; i < sd::kt_ncol; ++i) if (lx == kt_border_cx(kt, i) - kt.hscroll)
            {
                kt.col_w[(size_t)i] = std::clamp(kt_col_content_w(st, i) + 1, sd::kt_col_min, sd::kt_col_max);
                return true;
            }
        return faux;
    }
    inline auto kt_on_wheel(settings_state& st, si32 whlsi, bool hzwhl) -> void
    {
        auto& kt = st.kt;
        auto n = (si32)st.draft.keyfiles.size();
        if (hzwhl || kt.hsb_hover) kt.hscroll = std::clamp(kt.hscroll - whlsi * 4, 0, std::max(0, kt.content_w - kt.disp_w));
        else                       kt.scroll  = std::clamp(kt.scroll  - whlsi,     0, std::max(0, n - kt.rows));
    }
    // Hover feedback for both scrollbars and the column borders (table-local lx,ly).
    inline auto kt_on_move(settings_state& st, si32 lx, si32 ly) -> bool
    {
        auto& kt = st.kt;
        auto n = (si32)st.draft.keyfiles.size();
        auto dirty = faux;
        auto vsb = kt_vsb(kt, n);
        auto vh = vsb.ok && lx == vsb.x && ly >= vsb.top && ly < vsb.top + vsb.track_h;
        if (vh != kt.vsb_hover) { kt.vsb_hover = vh; dirty = true; }
        auto hsb = kt_hsb(kt);
        auto hh = hsb.ok && ly == hsb.top && lx >= hsb.x && lx < hsb.x + hsb.track_h;
        if (hh != kt.hsb_hover) { kt.hsb_hover = hh; dirty = true; }
        auto nb = si32{ -1 };
        if (ly >= 0 && ly < kt.div_bottom)
            for (auto i = si32{}; i < sd::kt_ncol; ++i) if (lx == kt_border_cx(kt, i) - kt.hscroll) { nb = i; break; }
        if (nb != kt.hover_border) { kt.hover_border = nb; dirty = true; }
        return dirty;
    }
    inline auto kt_clear_hover(settings_state& st) -> bool
    {
        auto& kt = st.kt;
        auto dirty = kt.vsb_hover || kt.hsb_hover || kt.hover_border != -1;
        kt.vsb_hover = kt.hsb_hover = faux; kt.hover_border = -1;
        return dirty;
    }
    // Decide the drag mode from the press location (table-local px,py): scrollbar thumb/rail,
    // a column border, or a rubber-band over the body. Returns true if the table claimed the drag.
    inline auto kt_drag_start(settings_state& st, si32 px, si32 py, bool ctl) -> bool
    {
        auto& kt = st.kt;
        auto n = (si32)st.draft.keyfiles.size();
        if (auto sb = kt_vsb(kt, n); sb.ok && px == sb.x && py >= sb.top && py < sb.top + sb.track_h)
        {
            if (py >= sb.thumb_y && py < sb.thumb_y + sb.thumb_h) kt.vsb_grab = py - sb.thumb_y;
            else { kt.vsb_grab = sb.thumb_h / 2; kt_vsb_scroll_to(kt, py, sb); }
            kt.vsb_drag = kt.vsb_hover = true; kt.drag_mode = 2; return true;
        }
        if (auto sb = kt_hsb(kt); sb.ok && py == sb.top && px >= sb.x && px < sb.x + sb.track_h)
        {
            auto tx = sb.x + sb.thumb_y;
            if (px >= tx && px < tx + sb.thumb_h) kt.hsb_grab = px - tx;
            else { kt.hsb_grab = sb.thumb_h / 2; kt_hsb_scroll_to(kt, px, sb); }
            kt.hsb_drag = kt.hsb_hover = true; kt.drag_mode = 3; return true;
        }
        if (py >= 0 && py < kt.div_bottom)
            for (auto i = si32{}; i < sd::kt_ncol; ++i) if (px == kt_border_cx(kt, i) - kt.hscroll)
            {
                kt.col_drag = i; kt.drag_mode = 1; return true;
            }
        if (py >= 1 && py < 1 + kt.rows)
        {
            kt.rubber_a = kt.rubber_b = std::max(0, kt.scroll + (py - 1));
            kt.rubber = true; kt.drag_mode = 4;
            kt.rubber_ctrl = ctl;
            if (kt.rubber_ctrl) { kt.rubber_add = !kt.drag_base.count(kt.rubber_a); kt.marked = kt.drag_base; }
            else                kt.marked.clear();
            if (kt.rubber_a < n) { kt.sel = kt.rubber_a; kt.sel_anchor = kt.rubber_a; }
            return true;
        }
        return faux;
    }
    inline auto kt_drag_pull(settings_state& st, si32 lx, si32 ly) -> bool
    {
        auto& kt = st.kt;
        auto n = (si32)st.draft.keyfiles.size();
        if (kt.drag_mode == 2)      { kt_vsb_scroll_to(kt, ly, kt_vsb(kt, n)); return true; }
        else if (kt.drag_mode == 3) { kt_hsb_scroll_to(kt, lx, kt_hsb(kt));    return true; }
        else if (kt.drag_mode == 1 && kt.col_drag >= 0)
        {
            auto left = kt_col_x(kt, kt.col_drag);
            kt.col_w[(size_t)kt.col_drag] = std::clamp(lx + kt.hscroll - left + 1, sd::kt_col_min, sd::kt_col_max);
            return true;
        }
        else if (kt.drag_mode == 4 && kt.rubber)
        {
            kt.rubber_b = kt.scroll + std::clamp(ly - 1, 0, std::max(0, kt.rows - 1));
            auto lo = std::min(kt.rubber_a, kt.rubber_b), hi = std::max(kt.rubber_a, kt.rubber_b);
            if (kt.rubber_ctrl)
            {
                kt.marked = kt.drag_base;
                for (auto i = lo; i <= hi; ++i) if (i >= 0 && i < n) { if (kt.rubber_add) kt.marked.insert(i); else kt.marked.erase(i); }
            }
            else { kt.marked.clear(); for (auto i = lo; i <= hi; ++i) if (i >= 0 && i < n) kt.marked.insert(i); }
            return true;
        }
        return faux;
    }
    inline auto kt_drag_end(settings_state& st) -> bool
    {
        auto& kt = st.kt;
        auto active = kt.drag_mode != 0;
        kt.vsb_drag = kt.hsb_drag = faux; kt.col_drag = -1; kt.rubber = faux; kt.drag_mode = 0;
        return active;
    }

    // Close the settings overlay and clear the window's "dialog active" guard.
    inline void sd_close(settings_state& st)
    {
        if (auto w = st.window_wp.lock()) w->base::property("parvion.settings.active", faux) = faux;
        if (auto o = st.overlay_wp.lock()) o->base::detach();
    }
    // Commit the edited draft to the controller (persists + applies to the live engine) and close.
    inline void sd_accept(settings_state& st)
    {
        st.harvest(); // Gather edited fields into draft (keyfiles were mutated in place by Add/Remove).
        if (st.ctrl) st.ctrl->update_settings(st.draft); // Stores cfg, applies onto the engine, saves + logs.
        sd_close(st);
    }

    // Paint one "Label  [field]  (hint)" row with the field anchored at a fixed column `fx`
    // (so every field in a group box lines up vertically). `cr` is the box content's right
    // edge (exclusive): the label and hint are clipped to it so nothing bleeds past the box
    // border when the dialog is forced below its negotiated minimum width. Caches the field's
    // input box for hit-testing and reuses paint_field so the input matches the Connect bar.
    inline void sd_field_row(auto& canvas, settings_state& st, si32 x, si32 y, si32 fx, si32 cr,
                             view label, sd::field_t fi, view hint)
    {
        put_str(canvas, x, y, label, theme::text_fg, theme::bg, std::max(0, std::min(fx - 1, cr) - x));
        auto fw = sd::field_w[fi];
        auto& f = st.fields[fi];
        f.box = rect{{ fx, y }, { fw, 1 }};
        auto active = st.focused && st.active == (si32)fi;
        paint_field(canvas, f.box, f.val, f.caret, f.off, active, faux);
        if (hint.size()) { auto hx = fx + fw + 2; put_str(canvas, hx, y, hint, theme::subtext, theme::bg, std::max(0, cr - hx)); }
    }

    // Paint wrapped help text starting at (x,y) within `maxw`; returns the next free row.
    inline auto sd_help(auto& canvas, si32 x, si32 y, si32 maxw, view s) -> si32
    {
        for (auto& ln : sd_wrap(s, maxw)) put_str(canvas, x, y++, ln, theme::subtext, theme::bg, maxw);
        return y;
    }

    inline void settings_render(settings_state& st, auto& canvas, twod sz)
    {
        auto W = sz.x, H = sz.y;
        if (W <= 4 || H <= 6) return;
        canvas.fill(rect{{ 0, 0 }, { W, H }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::text_fg); });
        // Title strip.
        canvas.fill(rect{{ 0, 0 }, { W, 1 }}, [&](cell& c){ c.bgc(theme::surface); });
        put_str(canvas, 1, 0, "Settings", theme::title_fg, theme::surface, W - 2);
        // Top tab strip — tabs sit flush from the left with no leading blank and no gap
        // between them (matching the queue panel's bottom tab strip: x starts at 0, each
        // tab is label+2 wide with one padding cell per side, and x advances by exactly bw).
        canvas.fill(rect{{ 0, 1 }, { W, 1 }}, [&](cell& c){ c.bgc(theme::header); });
        auto labels = std::array<view, sd::tab_count>{ "Connection", "SFTP" };
        auto tx = si32{ 0 };
        for (auto i = si32{}; i < sd::tab_count; ++i)
        {
            auto bw = (si32)cell_width(labels[i]) + 2; // One cell of padding on each side.
            st.hit.tab_box[i] = rect{{ tx, 1 }, { bw, 1 }};
            auto on = st.tab == i;
            auto fg = on ? ui32{ theme::title_fg_act } : ui32{ theme::subtext };
            auto bg = on ? ui32{ theme::surface }      : ui32{ theme::header };
            canvas.fill(st.hit.tab_box[i], [&](cell& c){ c.bgc(bg); });
            put_str(canvas, tx + 1, 1, labels[i], fg, bg, bw);
            tx += bw;
        }
        auto ix = si32{ sd::pad_x };           // Content left.
        auto iw = W - 2 * sd::pad_x;            // Content width.
        auto inner = iw - 4;                    // Inside a group box (border + 1 pad each side).
        auto cr = ix + 2 + inner;              // Box content right edge (exclusive): clip content here.
        // --- Connection tab -----------------------------------------------------------
        if (st.tab == sd::tab_connection)
        {
            auto y = si32{ 3 };                 // Blank row at y=2 (above first group box).
            // Timeout group — box height adapts to its content (1 field row + wrapped help).
            {
                auto hl = (si32)sd_wrap(sd::help_timeout, inner).size();
                auto bh = 2 + 1 + hl;           // borders + field row + help lines.
                sd_box(canvas, rect{{ ix, y }, { iw, bh }}, "Timeout");
                auto fx = ix + 2 + (si32)cell_width(sd::lbl_timeout) + 1;
                sd_field_row(canvas, st, ix + 2, y + 1, fx, cr, sd::lbl_timeout, sd::f_timeout, sd::hnt_timeout);
                sd_help(canvas, ix + 2, y + 2, inner, sd::help_timeout);
                y += bh + 1;                    // box + 1 blank row.
            }
            // Reconnection group — two fields aligned to the longer label, then wrapped help.
            {
                auto hl = (si32)sd_wrap(sd::help_reconn, inner).size();
                auto bh = 2 + 2 + hl;           // borders + 2 field rows + help lines.
                sd_box(canvas, rect{{ ix, y }, { iw, bh }}, "Reconnection settings");
                auto fx = ix + 2 + std::max((si32)cell_width(sd::lbl_retries),
                                            (si32)cell_width(sd::lbl_delay)) + 1;
                sd_field_row(canvas, st, ix + 2, y + 1, fx, cr, sd::lbl_retries, sd::f_retries, sd::hnt_retries);
                sd_field_row(canvas, st, ix + 2, y + 2, fx, cr, sd::lbl_delay,   sd::f_delay,   sd::hnt_delay);
                sd_help(canvas, ix + 2, y + 3, inner, sd::help_reconn);
            }
        }
        // --- SFTP tab -----------------------------------------------------------------
        else
        {
            // Stack the lower group boxes from the bottom (content-sized) and let the Public Key
            // Authentication box expand to fill the remaining space (its key table grows with it).
            auto par_h   = si32{ 2 + 2 };       // Parallel transfers: borders + 2 field rows.
            auto other_h = si32{ 2 + 1 };       // Other SFTP options: borders + checkbox row.
            auto hash_h  = si32{ 2 + 1 };       // Hash verification: borders + one dropdown row.
            auto par_y   = (H - 1) - 1 - par_h; // Above the (blank + button) rows.
            auto other_y = par_y - 1 - other_h;
            auto hash_y  = other_y - 1 - hash_h;
            auto pk_y    = si32{ 3 };
            auto pk_h    = std::max(6, (hash_y - 1) - pk_y); // Fill the gap above Hash verification.

            // Public Key Authentication group.
            sd_box(canvas, rect{{ ix, pk_y }, { iw, pk_h }}, "Public Key Authentication");
            auto py = sd_help(canvas, ix + 2, pk_y + 1, inner, sd::help_pubkey);
            put_str(canvas, ix + 2, py, sd::lbl_privkeys, theme::text_fg, theme::bg, inner);
            // Add / Remove buttons sit on the box's last inner row; the table fills the rest.
            auto btn_y = pk_y + pk_h - 2;
            auto tbl_top = py + 1;
            auto tbl_h = std::max(2, btn_y - 1 - tbl_top);
            st.kt.area = rect{{ ix + 2, tbl_top }, { inner, tbl_h }};
            kt_render(canvas, st, st.focused);
            st.hit.addkey    = rect{{ ix + 2, btn_y }, { (si32)cell_width(sd::btn_addkey), 1 }};
            st.hit.removekey = rect{{ st.hit.addkey.coor.x + st.hit.addkey.size.x + 1, btn_y }, { (si32)cell_width(sd::btn_removekey), 1 }};
            paint_button(canvas, st.hit.addkey,    sd::btn_addkey,    st.hover_add,    st.press_add);
            paint_button(canvas, st.hit.removekey, sd::btn_removekey, st.hover_remove, st.press_remove);

            // Other SFTP options group (compression checkbox).
            sd_box(canvas, rect{{ ix, other_y }, { iw, other_h }}, "Other SFTP options");
            st.hit.compression = rect{{ ix + 2, other_y + 1 }, { inner, 1 }};
            put_str(canvas, ix + 2, other_y + 1, st.compression ? "\xE2\x96\xA3 Enable compression" : "\xE2\x96\xA1 Enable compression", theme::text_fg, theme::bg, inner);

            // Hash verification group: a single dropdown selects the transfer hash algorithm;
            // "None" (the default) disables hashing during transfers.
            sd_box(canvas, rect{{ ix, hash_y }, { iw, hash_h }}, "Hash verification");
            put_str(canvas, ix + 2, hash_y + 1, sd::lbl_hash_xfer, theme::text_fg, theme::bg, std::max(0, std::min((si32)cell_width(sd::lbl_hash_xfer), cr - (ix + 2))));
            auto hax     = ix + 2 + (si32)cell_width(sd::lbl_hash_xfer) + 1;
            auto halabel = text{ " " } + text{ st.hash_on_transfer ? hash_algo_label(st.hash_algo) : sd::hash_none } + " \xE2\x96\xBE "; // " None ▾ " / " SHA-256 ▾ "
            st.hit.hash_algo = rect{{ hax, hash_y + 1 }, { std::max(0, std::min((si32)cell_width(halabel), cr - hax)), 1 }};
            paint_button(canvas, st.hit.hash_algo, halabel, st.hover_hashalgo, faux);

            // Parallel transfers group — both input fields aligned to the longer label (item 4).
            // Label/field/control all clip to the box content edge (cr) so nothing bleeds out.
            sd_box(canvas, rect{{ ix, par_y }, { iw, par_h }}, "Parallel transfers");
            auto fx = ix + 2 + std::max((si32)cell_width(sd::lbl_threshold), (si32)cell_width(sd::lbl_maxconn)) + 1;
            // Row 1: threshold value field + unit dropdown.
            put_str(canvas, ix + 2, par_y + 1, sd::lbl_threshold, theme::text_fg, theme::bg, std::max(0, std::min(fx - 1, cr) - (ix + 2)));
            st.fields[sd::f_threshold].box = rect{{ fx, par_y + 1 }, { sd::field_w[sd::f_threshold], 1 }};
            paint_field(canvas, st.fields[sd::f_threshold].box, st.fields[sd::f_threshold].val, st.fields[sd::f_threshold].caret, st.fields[sd::f_threshold].off, st.focused && st.active == sd::f_threshold, faux);
            auto ux = fx + sd::field_w[sd::f_threshold] + 2;
            auto ulabel = text{ " " } + text{ sftp_unit_label(st.threshold_unit) } + " \xE2\x96\xBE "; // " MiB ▾ "
            st.hit.unit = rect{{ ux, par_y + 1 }, { std::max(0, std::min((si32)cell_width(ulabel), cr - ux)), 1 }};
            paint_button(canvas, st.hit.unit, ulabel, st.hover_unit, faux);
            // Row 2: max connections field (aligned to fx) + range hint.
            put_str(canvas, ix + 2, par_y + 2, sd::lbl_maxconn, theme::text_fg, theme::bg, std::max(0, std::min(fx - 1, cr) - (ix + 2)));
            st.fields[sd::f_maxconn].box = rect{{ fx, par_y + 2 }, { sd::field_w[sd::f_maxconn], 1 }};
            paint_field(canvas, st.fields[sd::f_maxconn].box, st.fields[sd::f_maxconn].val, st.fields[sd::f_maxconn].caret, st.fields[sd::f_maxconn].off, st.focused && st.active == sd::f_maxconn, faux);
            { auto hx = fx + sd::field_w[sd::f_maxconn] + 2; put_str(canvas, hx, par_y + 2, sd::hnt_maxconn, theme::subtext, theme::bg, std::max(0, cr - hx)); }
        }
        // --- Button row (OK + Cancel; right-aligned block) -----------------------------
        auto okw = si32{ 4 }, cnw = si32{ 8 };
        auto by = H - 1;
        auto cx = W - 1 - cnw;
        st.hit.cancel = rect{{ cx, by }, { cnw, 1 }};
        st.hit.ok     = rect{{ cx - 1 - okw, by }, { okw, 1 }};
        paint_button(canvas, st.hit.ok,     " OK ",     st.hover_ok,     st.press_ok);
        paint_button(canvas, st.hit.cancel, " Cancel ", st.hover_cancel, st.press_cancel);
    }

    // Insert a digit string into the active numeric field (digits only, like the Port field).
    inline void sd_field_insert(settings_state& st, view ins)
    {
        if (st.active < 0 || st.active >= sd::f_count) return;
        auto digits = text{};
        for (auto c : ins) if (c >= '0' && c <= '9') digits += c;
        auto& f = st.fields[st.active];
        edit_insert(f.val, f.caret, digits);
    }

    // sd_hit (point-in-rect) now lives in panes.hpp so the secret-prompt modal can share it.

    // Append a parsed private key to the draft table. `path` is the original key, or — for an
    // encrypted non-ppk key — its converted .ppk. De-dups and selects the new row.
    inline void sd_store_key(settings_state& st, text const& path, text const& comment, text const& data)
    {
        for (auto& k : st.draft.keyfiles) if (k == path) return; // No duplicates.
        st.draft.keyfiles.push_back(path);
        st.key_comment.push_back(comment);
        st.key_data.push_back(data);
        st.kt.sel = (si32)st.draft.keyfiles.size() - 1;
        st.kt.marked = { st.kt.sel };
        if (st.ctrl) st.ctrl->log_line(logtype::status, "Added key file " + path + (comment.empty() ? text{} : " (" + comment + ")"));
    }

    // Forward declaration so the conversion can re-trigger the passphrase prompt on a failed attempt.
    inline void sd_begin_convert(settings_state& st, text const& path, bool retry = faux);

    // FileZilla CFZPuttyGenInterface::LoadKeyFile conversion path: an encrypted non-PuTTY key
    // ("convertible") must be converted to a .ppk before the backend can use it. Verifying the
    // passphrase and converting the key are the SAME expensive operation (decrypt the key, ~1s), so we
    // do it ONCE: right after the passphrase, the key is decrypted and the .ppk is written to a TEMP
    // file (off the UI thread, behind a spinner — the TUI never freezes). A wrong passphrase fails here
    // and re-prompts early, before the picker. The remaining "Save converted key" step just COPIES the
    // ready .ppk to the chosen path (instant) — no second decrypt. The temp .ppk is encrypted with the
    // same passphrase (safe on disk) and is removed after Save / cancel. The settings card outlives
    // these overlays (it stays attached beneath), so capturing &st is safe for the whole flow.
    inline void sd_begin_convert(settings_state& st, text const& path, bool retry)
    {
        auto window = st.window_wp.lock();
        if (!window) return;
        auto stp = &st;
        auto window_wp = st.window_wp;
        auto card_wp   = st.card_wp;
        auto base   = fs::path{ path }.filename().string();
        auto prompt = "Enter the passphrase for \"" + base + "\". The key will be converted to PuTTY (.ppk) format, protected with the same passphrase.";
        auto on_cancel = [stp]{ if (stp->ctrl) stp->ctrl->log_line(logtype::status, "Key conversion cancelled."); };
        auto on_pass = [stp, path, window_wp, card_wp](text pass)
        {
            // Decrypt + write the .ppk to a temp file ONCE (async). This both verifies the passphrase
            // (wrong -> re-prompt) and does all the heavy crypto a single time.
            auto ec = std::error_code{};
            auto tmpdir = fs::temp_directory_path(ec);
            if (ec) tmpdir = fs::path{ "/tmp" };
            static std::atomic<uint64_t> seq{ 0 };
            auto tmp = (tmpdir / ("parvion-convert-" + std::to_string(seq.fetch_add(1)) + ".ppk")).string();
            run_with_progress(window_wp, card_wp, {}, "Converting key...",
                [path, pass, tmp](std::vector<text>& reps){ return pvputtygen_run("file " + path + "\npassword " + pass + "\nwrite " + tmp + "\nfingerprint\ncomment\n\n", reps); },
                [stp, path, tmp, window_wp, card_wp](std::vector<text> reps, bool ok)
                {
                    if (!ok || reps.size() < 5) // Wrong passphrase (or write error): clean up + re-prompt.
                    {
                        auto e = std::error_code{}; fs::remove(fs::path{ tmp }, e);
                        sd_begin_convert(*stp, path, /*retry*/true);
                        return;
                    }
                    // Replies in order: file (convertible), password, write, fingerprint, comment.
                    auto data = reps[3], comment = reps[4];
                    // Only the Save step remains: pick the output path; copying the ready .ppk is instant.
                    auto def_dir  = fs::path{ path }.parent_path().string();
                    auto def_name = fs::path{ path }.filename().replace_extension(".ppk").string();
                    open_file_picker(window_wp, card_wp, {}, picker_mode::save, "Save converted key", def_dir, def_name,
                        [stp, path, tmp, comment, data](text const& chosen)
                        {
                            auto ec1 = std::error_code{};
                            fs::copy_file(fs::path{ tmp }, fs::path{ chosen }, fs::copy_options::overwrite_existing, ec1);
                            auto ec2 = std::error_code{}; fs::remove(fs::path{ tmp }, ec2);
                            if (ec1) { if (stp->ctrl) stp->ctrl->log_line(logtype::error, "Could not save converted key to " + chosen); return; }
                            sd_store_key(*stp, chosen, comment, data); // comment, data (fingerprint) from the convert step.
                            if (stp->ctrl) stp->ctrl->log_line(logtype::status, "Converted " + path + " -> " + chosen);
                        },
                        [stp, tmp]{ auto e = std::error_code{}; fs::remove(fs::path{ tmp }, e); // Cancel: drop the temp .ppk.
                                    if (stp->ctrl) stp->ctrl->log_line(logtype::status, "Key conversion cancelled."); });
                });
        };
        window->base::attach(make_secret_dialog(window_wp, card_wp, "Convert private key", prompt, on_pass, on_cancel, retry));
    }

    // Add a private key (FileZilla CFZPuttyGenInterface::LoadKeyFile): classify via pvputtygen and
    // either store a usable key as-is, convert an encrypted non-ppk key, or reject (SSH1/unreadable).
    inline void sd_add_key(settings_state& st, text const& path)
    {
        if (path.empty()) return;
        for (auto& k : st.draft.keyfiles) if (k == path) return; // No duplicates.
        auto rs = std::vector<text>{};
        pvputtygen_run("file " + path + "\nencrypted\n\n", rs);
        auto kind = rs.empty() ? text{} : rs[0];
        if (kind == "ok") // Native .ppk or an unencrypted importable key: usable as-is.
        {
            auto comment = text{}, data = text{};
            sd_keyinfo(path, comment, data);
            sd_store_key(st, path, comment, data);
        }
        else if (kind == "convertible") sd_begin_convert(st, path, faux); // Encrypted non-ppk: convert to .ppk.
        else if (st.ctrl)
        {
            st.ctrl->log_line(logtype::error, kind == "incompatible"
                ? text{ "SSH1 keys are not supported (SSH2 only): " + path }
                : text{ "Could not load or parse private key: " + path });
        }
    }
    // Remove the selected key rows from the draft table.
    inline void sd_remove_keys(settings_state& st)
    {
        auto sel = st.kt.marked;
        if (sel.empty() && st.kt.sel >= 0) sel.insert(st.kt.sel);
        if (sel.empty()) return;
        auto nk = std::vector<text>{}, nc = std::vector<text>{}, nd = std::vector<text>{};
        for (auto i = si32{}; i < (si32)st.draft.keyfiles.size(); ++i) if (!sel.count(i))
        {
            nk.push_back(st.draft.keyfiles[i]);
            nc.push_back(i < (si32)st.key_comment.size() ? st.key_comment[i] : text{});
            nd.push_back(i < (si32)st.key_data.size()    ? st.key_data[i]    : text{});
        }
        st.draft.keyfiles = nk; st.key_comment = nc; st.key_data = nd;
        st.kt.sel = -1; st.kt.marked.clear(); st.kt.scroll = 0;
        if (st.ctrl) st.ctrl->log_line(logtype::status, "Removed selected private key(s).");
    }

    // Build the threshold-unit dropdown menu (item 3): one radio row per unit (Byte..TiB). The
    // selected row is published via radio_checked; each row's action sets st.threshold_unit.
    inline auto sd_build_unit_menu(settings_state& st, netxs::wptr<ui::base> card_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto items = std::vector<m::item>{};
        auto stp = &st;
        for (auto i = si32{}; i < sftp_unit_count; ++i)
        {
            auto row = m::item{ .alive = true, .label = text{ sftp_unit_label(i) }, .checked = (i == st.threshold_unit) };
            row.action = [stp, card_wp, i](hids&)
            {
                stp->threshold_unit = i;
                if (auto c = card_wp.lock()) c->base::deface();
            };
            items.push_back(std::move(row));
        }
        return items;
    }

    // Transfer-hash dropdown (Settings -> SFTP -> Hash verification): "None" disables hashing on
    // transfer; any algorithm enables it with that algorithm. Index 0 = None, 1..N = algorithms.
    inline auto sd_build_hash_menu(settings_state& st, netxs::wptr<ui::base> card_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto items = std::vector<m::item>{};
        auto stp = &st;
        auto deface = [card_wp]{ if (auto c = card_wp.lock()) c->base::deface(); };
        auto none = m::item{ .alive = true, .label = text{ sd::hash_none }, .checked = !st.hash_on_transfer };
        none.action = [stp, deface](hids&){ stp->hash_on_transfer = faux; deface(); };
        items.push_back(std::move(none));
        for (auto i = si32{}; i < hash_algo_count; ++i)
        {
            auto row = m::item{ .alive = true, .label = text{ hash_algo_label(i) }, .checked = (st.hash_on_transfer && i == st.hash_algo) };
            row.action = [stp, deface, i](hids&){ stp->hash_on_transfer = true; stp->hash_algo = i; deface(); };
            items.push_back(std::move(row));
        }
        return items;
    }

    // "Add key file..." picker: the reusable Open-mode file picker (panes.hpp open_file_picker),
    // seeded at the user's home and Windows drive-aware. Activating a file (double-click / Enter /
    // Open) adds it as a key; an encrypted non-ppk key then converts (see sd_add_key). The settings
    // card outlives the picker (it stays attached beneath), so capturing &st in on_accept is safe.
    inline void sd_open_key_picker(settings_state& st, id_t gear_id = {})
    {
        open_file_picker(st.window_wp, st.card_wp, gear_id, picker_mode::open, "Add key file", user_home_dir(), {},
                         [stp = &st](text const& path){ sd_add_key(*stp, path); });
    }

    // Build the Settings dialog overlay (dimming scrim + centered self-handling card).
    // Build the Settings dialog overlay. `card_out` (when non-null) receives the inner card
    // widget so the caller can grab keyboard focus on it after attaching (see OpenSettingsDialog).
    inline auto make_settings_dialog(sftp_remote* ctrl, netxs::wptr<ui::base> window_wp, ui::sptr* card_out = nullptr) -> ui::sptr
    {
        auto overlay = ui::cake::ctor()->alignment({ snap::both, snap::both });
        auto overlay_wp = ptr::shadow(overlay);
        // Dimming backdrop (click outside cancels).
        overlay->attach(ui::mock::ctor())->invoke([overlay_wp, window_wp](auto& boss)
        {
            auto myid = boss.bell::id;
            boss.LISTEN(tier::release, e2::render::background::any, parent_canvas, -, (myid))
            {
                parent_canvas.fill([myid](cell& c){ c.bgc().faint(); c.fgc().faint(); c.link(myid); });
            };
            boss.on(tier::mouserelease, input::key::LeftClick, [overlay_wp, window_wp](hids& gear)
            {
                if (auto w = window_wp.lock()) w->base::property("parvion.settings.active", faux) = faux;
                if (auto o = overlay_wp.lock()) o->base::detach();
                gear.dismiss();
            });
        });
        // Centered card.
        // The card's minimum width is negotiated from the form's internal components (see
        // sd_min_width): wide enough that no group-box content ever overflows its border.
        auto minw = sd_min_width();
        auto card = overlay->attach(ui::mock::ctor())
            ->active()
            ->alignment({ snap::center, snap::center })
            ->limits({ minw, 24 }, { std::max(minw, 104), 42 })
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(pro::focus::mode::focused)
            ->plugin<pro::keybd>();
        card->invoke([ctrl, window_wp, overlay_wp](auto& boss)
        {
            auto& st = boss.base::field(settings_state{});
            st.ctrl = ctrl;
            st.window_wp = window_wp;
            st.overlay_wp = overlay_wp;
            st.card_wp = ptr::shadow(boss.This());
            st.seed();
            // Re-parse every persisted key so its Comment/Data populate the table on every open
            // (FileZilla re-runs LoadKeyFile for each key when the SFTP page is shown). Without
            // this, reopening Settings showed blank Comment/Data for already-added keys.
            for (auto i = size_t{}; i < st.draft.keyfiles.size(); ++i)
            {
                auto comment = text{}, data = text{};
                sd_keyinfo(st.draft.keyfiles[i], comment, data);
                if (i < st.key_comment.size()) st.key_comment[i] = comment;
                if (i < st.key_data.size())    st.key_data[i]    = data;
            }

            boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);
            boss.LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                settings_render(st, parent_canvas, boss.base::size());
            };
            boss.LISTEN(tier::release, e2::form::state::focus::count, count)
            {
                st.focused = !!count;
                boss.base::deface();
            };
            // Caret-to-click for a field box (card-local mx).
            auto caret_to = [&](sd::field_t fi, si32 mx)
            {
                auto& f = st.fields[fi];
                f.caret = std::min(cell_to_cluster(f.val, f.off + (mx - f.box.coor.x)), cluster_count(f.val));
            };
            boss.on(tier::mouserelease, input::key::LeftDown, [&, caret_to](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                // Tabs.
                for (auto i = si32{}; i < sd::tab_count; ++i) if (sd_hit(st.hit.tab_box[i], mx, my))
                {
                    if (st.tab != i) { st.tab = i; st.active = -1; }
                    boss.base::deface(); gear.dismiss(); return;
                }
                // Buttons: arm press (fire on click).
                if (sd_hit(st.hit.ok, mx, my))        st.press_ok = true;
                if (sd_hit(st.hit.cancel, mx, my))    st.press_cancel = true;
                if (st.tab == sd::tab_sftp)
                {
                    if (sd_hit(st.hit.addkey, mx, my))    st.press_add = true;
                    if (sd_hit(st.hit.removekey, mx, my)) st.press_remove = true;
                    if (sd_hit(st.hit.compression, mx, my)) { st.compression = !st.compression; }
                    // Key-table row selection (the press; scrollbar/border presses go to drag/click).
                    if (kt_in_area(st, mx, my))
                    {
                        auto ctl  = !!(gear.ctlstat & hids::anyCtrl);
                        auto shft = !!(gear.ctlstat & hids::anyShift);
                        kt_on_down(st, mx - st.kt.area.coor.x, my - st.kt.area.coor.y, ctl, shft);
                    }
                }
                // Field activation (current tab only).
                for (auto i = si32{}; i < sd::f_count; ++i) if (st.fields[i].tab == st.tab && sd_hit(st.fields[i].box, mx, my))
                {
                    st.active = i;
                    caret_to((sd::field_t)i, mx);
                }
                boss.base::deface();
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                auto fired = faux;
                if (sd_hit(st.hit.ok, mx, my))     { sd_accept(st); fired = true; }
                else if (sd_hit(st.hit.cancel, mx, my)) { sd_close(st); fired = true; }
                else if (st.tab == sd::tab_sftp)
                {
                    if (sd_hit(st.hit.addkey, mx, my))    { sd_open_key_picker(st, gear.id); fired = true; }
                    else if (sd_hit(st.hit.removekey, mx, my)) { sd_remove_keys(st); fired = true; }
                    else if (sd_hit(st.hit.unit, mx, my)) // Open the threshold-unit dropdown (item 3).
                    {
                        auto at = twod{ st.hit.unit.coor.x, st.hit.unit.coor.y + 1 };
                        app::shared::menu::open_dropdown_popup(boss, sd_build_unit_menu(st, st.card_wp), true, st.threshold_unit, at);
                        fired = true;
                    }
                    else if (sd_hit(st.hit.hash_algo, mx, my)) // Open the transfer-hash dropdown (None + algorithms).
                    {
                        auto at  = twod{ st.hit.hash_algo.coor.x, st.hit.hash_algo.coor.y + 1 };
                        auto sel = st.hash_on_transfer ? st.hash_algo + 1 : 0; // 0 = None.
                        app::shared::menu::open_dropdown_popup(boss, sd_build_hash_menu(st, st.card_wp), true, sel, at);
                        fired = true;
                    }
                    else if (kt_in_area(st, mx, my)) // Scrollbar rail paging.
                    {
                        if (kt_on_click(st, mx - st.kt.area.coor.x, my - st.kt.area.coor.y)) fired = true;
                    }
                }
                st.press_ok = st.press_cancel = st.press_add = st.press_remove = faux;
                if (!fired) boss.base::deface();
                else boss.base::deface();
            });
            boss.on(tier::mouserelease, input::key::LeftDoubleClick, [&](hids& gear)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (st.tab == sd::tab_sftp && kt_in_area(st, mx, my)
                    && kt_on_dclick(st, mx - st.kt.area.coor.x, my - st.kt.area.coor.y)) { boss.base::deface(); gear.dismiss(); }
            });
            boss.on(tier::mouserelease, input::key::MouseWheel, [&](hids& gear)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (st.tab == sd::tab_sftp && kt_in_area(st, mx, my))
                {
                    kt_on_wheel(st, gear.whlsi, gear.hzwhl);
                    boss.base::deface();
                    gear.dismiss();
                }
            });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&)
            {
                if (kt_clear_hover(st)) boss.base::deface();
            });
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids& gear)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                auto dirty = faux;
                auto upd = [&](bool& h, rect const& b){ auto o = sd_hit(b, mx, my); if (h != o) { h = o; dirty = true; } };
                upd(st.hover_ok, st.hit.ok);
                upd(st.hover_cancel, st.hit.cancel);
                if (st.tab == sd::tab_sftp)
                {
                    upd(st.hover_add, st.hit.addkey);
                    upd(st.hover_remove, st.hit.removekey);
                    upd(st.hover_unit, st.hit.unit);
                    upd(st.hover_hashalgo, st.hit.hash_algo);
                    if (kt_in_area(st, mx, my)) { if (kt_on_move(st, mx - st.kt.area.coor.x, my - st.kt.area.coor.y)) dirty = true; }
                    else if (kt_clear_hover(st)) dirty = true;
                }
                if (dirty) boss.base::deface();
            });
            // Left-drag: a press within the key table drives its column resize / scrollbar / rubber-band
            // selection; otherwise it scrubs the active field's caret (mutually exclusive by press loc).
            boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear)
            {
                auto px = (si32)gear.click.x, py = (si32)gear.click.y;
                st.drag_field = -1;
                st.kt.drag_mode = 0;
                if (st.tab == sd::tab_sftp && kt_in_area(st, px, py))
                {
                    auto ctl = !!(gear.ctlstat & hids::anyCtrl);
                    if (kt_drag_start(st, px - st.kt.area.coor.x, py - st.kt.area.coor.y, ctl)) { boss.base::deface(); return; }
                }
                for (auto i = si32{}; i < sd::f_count; ++i) if (st.fields[i].tab == st.tab && sd_hit(st.fields[i].box, px, py)) { st.drag_field = i; break; }
            };
            boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>, gear)
            {
                if (st.kt.drag_mode != 0)
                {
                    if (kt_drag_pull(st, (si32)gear.coord.x - st.kt.area.coor.x, (si32)gear.coord.y - st.kt.area.coor.y)) boss.base::deface();
                    return;
                }
                if (st.drag_field < 0) return;
                auto& f = st.fields[st.drag_field];
                f.caret = std::min(cell_to_cluster(f.val, f.off + ((si32)gear.coord.x - f.box.coor.x)), cluster_count(f.val));
                boss.base::deface();
            };
            boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>,   gear) { st.drag_field = -1; if (kt_drag_end(st)) boss.base::deface(); };
            boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear) { st.drag_field = -1; if (kt_drag_end(st)) boss.base::deface(); };
            boss.LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                if (!st.focused) return;
                if (gear.payload == input::keybd::type::keypaste)
                {
                    if (st.active >= 0) { sd_field_insert(st, edit_filter(gear.cluster)); boss.base::deface(); gear.set_handled(); }
                    return;
                }
                if (gear.payload != input::keybd::type::keypress) return;
                if (gear.keystat == input::key::released || gear.keystat == input::key::interrupted) return;
                if (gear.keybd::handled) return;
                auto k = gear.keybd::generic();
                auto shift = !!(gear.ctlstat & hids::anyShift);
                auto act = true;
                // Current tab's field order (for Tab navigation).
                auto order = st.tab == sd::tab_connection
                    ? std::vector<si32>{ sd::f_timeout, sd::f_retries, sd::f_delay }
                    : std::vector<si32>{ sd::f_threshold, sd::f_maxconn };
                if (k == input::key::Esc) { sd_close(st); }
                else if (k == input::key::KeyEnter) { sd_accept(st); }
                else if (k == input::key::Tab)
                {
                    auto pos = 0;
                    for (auto i = 0; i < (si32)order.size(); ++i) if (order[i] == st.active) pos = i;
                    auto n = (si32)order.size();
                    st.active = order[(pos + (shift ? n - 1 : 1)) % n];
                }
                else if (st.active >= 0)
                {
                    auto& f = st.fields[st.active];
                         if (k == input::key::Backspace)     edit_backspace(f.val, f.caret);
                    else if (k == input::key::KeyDelete)     edit_delete(f.val, f.caret);
                    else if (k == input::key::KeyLeftArrow)  f.caret = std::max(0, f.caret - 1);
                    else if (k == input::key::KeyRightArrow) f.caret = std::min(cluster_count(f.val), f.caret + 1);
                    else if (k == input::key::KeyHome)       f.caret = 0;
                    else if (k == input::key::KeyEnd)        f.caret = cluster_count(f.val);
                    else { auto ins = edit_filter(gear.cluster); if (ins.size()) sd_field_insert(st, ins); else act = faux; }
                }
                else act = faux;
                if (act) { gear.set_handled(); boss.base::deface(); }
            };
        });
        if (card_out) *card_out = card; // Expose the card so the caller can focus it after attach.
        return overlay;
    }
}
