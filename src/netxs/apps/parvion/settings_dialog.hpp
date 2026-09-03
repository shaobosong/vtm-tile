// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/settings_dialog.hpp: component-based Settings port.
//
// The previous full hand-painted implementation is retained verbatim as
// settings_dialog.hpp.old. The Settings pages and their child dialogs are
// composed from retained components.

#include "panes.hpp"
#include "components/button.hpp"
#include "components/checkbox.hpp"
#include "components/dialog.hpp"
#include "components/dropdown.hpp"
#include "components/flex.hpp"
#include "components/grid.hpp"
#include "components/groupbox.hpp"
#include "components/input.hpp"
#include "components/label.hpp"
#include "components/scrollview.hpp"
#include "components/table.hpp"
#include "components/tabs.hpp"
#include "file_picker_dialog.hpp"
#include "secret_dialog.hpp"

#if !defined(_WIN32)
    #include <sys/wait.h>
#endif

namespace netxs::app::parvion
{
    struct settings_dialog_state
    {
        sftp_remote* ctrl = nullptr;
        parvion_settings draft;
        text timeout;
        text reconnect_count;
        text reconnect_delay;
        text threshold_value;
        text max_connections;
        bool compression = faux;
        si32 threshold_unit = 2;
        si32 transfer_allocation = allocation_strict;
        conflict_policy_t conflict_policy = conflict_overwrite;
        bool hash_on_transfer = faux;
        si32 hash_algo = 2;
        std::array<si32, 3> key_column_widths{ 18, 18, 28 };
        std::array<bool, 3> key_columns_shown{ true, true, true };
        std::set<si32> key_selection;
        ui64 key_table_revision = 0;
        std::vector<text> key_comments;
        std::vector<text> key_data;
        netxs::wptr<ui::base> key_table_wp;
        netxs::wptr<ui::base> key_remove_button_wp;
        std::array<si32, 4> site_column_widths{ 20, 26, 8, 20 };
        std::array<bool, 4> site_columns_shown{ true, true, true, true };
        std::set<si32> site_selection;
        ui64 site_table_revision = 0;
        netxs::wptr<ui::base> site_table_wp;
        netxs::wptr<ui::base> site_edit_button_wp;
        netxs::wptr<ui::base> site_remove_button_wp;
        si32 log_debug_level = log_debug_none;
        bool log_raw_listing = faux;
        bool done = faux;
        netxs::wptr<ui::base> popup_wp;
        netxs::wptr<ui::base> window_wp;
    };

    namespace settings_connection
    {
        inline constexpr auto timeout_label = view{ "Timeout in seconds:" };
        inline constexpr auto timeout_range = view{ "(10-9999, 0 to disable)" };
        inline constexpr auto timeout_help = view{ "If no data is sent or received during an operation for longer than the specified time, the connection will be closed and Parvion will try to reconnect." };
        inline constexpr auto retries_label = view{ "Maximum number of retries:" };
        inline constexpr auto retries_range = view{ "(0-99, 0 for unlimited)" };
        inline constexpr auto delay_label = view{ "Delay between failed attempts:" };
        inline constexpr auto delay_range = view{ "(0-999 seconds)" };
        inline constexpr auto reconnect_help = view{ "Parvion will retry a dropped control connection and a transfer chunk whose connection was lost this many times, waiting the configured delay between attempts. Local failures (disk, launching the helper, missing passphrase) and server refusals (permission, missing file, remote quota) are not retried. Please note that some servers might ban you if you try to reconnect too often or in too short intervals." };
    }

    namespace settings_sftp
    {
        inline constexpr auto key_headers = std::array<view, 3>{ "Filename", "Comment", "Data" };
        inline constexpr auto key_menu_headers = std::array<view, 3>{ "&Filename", "&Comment", "&Data" };
        inline constexpr auto public_key_help = view{ "To support public key authentication, Parvion needs to know the private keys to use." };
        inline constexpr auto private_keys_label = view{ "Private keys:" };
        inline constexpr auto threshold_label = view{ "Enable parallel transfers for files larger than:" };
        inline constexpr auto max_connections_label = view{ "Maximum parallel transfer connections:" };
        inline constexpr auto allocation_label = view{ "Channel allocation:" };
        inline constexpr auto existing_policy_label = view{ "Existing files:" };
        inline constexpr auto max_connections_hint = view{ "(1-10)" };
        inline constexpr auto hash_label = view{ "Calculate target file hash during transfers:" };
        inline constexpr auto hash_none = view{ "None" };
    }

    namespace settings_site
    {
        enum field_t { name, host, port, user, password, field_count };
        inline constexpr auto headers = std::array<view, 4>{ "Site Name", "Host", "Port", "User" };
        inline constexpr auto menu_headers = std::array<view, 4>{ "Site &Name", "&Host", "&Port", "&User" };
        inline constexpr auto help = view{ "Save SFTP connection details for quick access from the connect bar." };
        inline constexpr auto sites_label = view{ "Saved SFTP sites:" };
        inline constexpr auto field_labels = std::array<view, 5>{
            "Site Name", "Host *", "Port", "User", "Password"
        };
    }

    namespace settings_debug
    {
        inline constexpr auto level_label = view{ "Debug information in message log:" };
        inline constexpr auto help = view{ "The higher the debug level, the more information will be displayed in the message log. Displaying debug information has a negative impact on performance. If reporting bugs, provide logs with Verbose logging level." };
    }

    inline auto make_settings_label(view value,
                                    label_role role = label_role::text,
                                    label_overflow overflow = label_overflow::clip,
                                    label_palette palette = {}) -> component
    {
        return make_label({
            .value = [value = text{ value }]{ return value; },
            .role = role,
            .overflow = overflow,
            .palette = palette,
        });
    }

    // Run the pvputtygen multicall helper and collect its fzprintf payloads.
    // The protocol and platform plumbing intentionally remain identical to
    // settings_dialog.hpp.old while the surrounding page is refactored.
    inline auto settings_pvputtygen_run(text const& script, std::vector<text>& replies) -> bool
    {
        replies.clear();
        auto out = text{};
    #if !defined(_WIN32)
        auto exe = os::process::binary();
        auto inpipe = std::array<int, 2>{};
        auto outpipe = std::array<int, 2>{};
        if (::pipe(inpipe.data()) != 0 || ::pipe(outpipe.data()) != 0) return faux;
        auto pid = ::fork();
        if (pid < 0) return faux;
        if (pid == 0)
        {
            ::dup2(inpipe[0], 0);
            ::dup2(outpipe[1], 1);
            ::close(inpipe[0]); ::close(inpipe[1]);
            ::close(outpipe[0]); ::close(outpipe[1]);
            ::execl(exe.c_str(), exe.c_str(), "-r", "pvputtygen", (char*)nullptr);
            ::_exit(127);
        }
        ::close(inpipe[0]); ::close(outpipe[1]);
        auto wr = ::write(inpipe[1], script.data(), script.size()); (void)wr;
        ::close(inpipe[1]);
        auto buf = std::array<char, 4096>{};
        for (auto n = ssize_t{}; (n = ::read(outpipe[0], buf.data(), buf.size())) > 0; )
            out.append(buf.data(), (size_t)n);
        ::close(outpipe[0]);
        auto status = 0;
        ::waitpid(pid, &status, 0);
    #else
        auto sa = SECURITY_ATTRIBUTES{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        auto in_rd = HANDLE{}, in_wr = HANDLE{}, out_rd = HANDLE{}, out_wr = HANDLE{};
        if (!::CreatePipe(&in_rd, &in_wr, &sa, 0)) return faux;
        if (!::CreatePipe(&out_rd, &out_wr, &sa, 0))
        {
            ::CloseHandle(in_rd); ::CloseHandle(in_wr);
            return faux;
        }
        ::SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
        auto nul = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                                 &sa, OPEN_EXISTING, 0, nullptr);
        auto startup = STARTUPINFOEXW{ sizeof(STARTUPINFOEXW) };
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = in_rd;
        startup.StartupInfo.hStdOutput = out_wr;
        startup.StartupInfo.hStdError = nul;
        HANDLE inherit[] = { in_rd, out_wr, nul };
        auto attrbuf = std::vector<byte>{};
        auto attrsize = SIZE_T{};
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrsize);
        attrbuf.resize(attrsize);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrbuf.data());
        auto process = PROCESS_INFORMATION{};
        auto command = "\"" + os::process::binary() + "\" -r pvputtygen";
        auto wide_command = utf::to_utf(command);
        auto ok = ::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attrsize)
               && ::UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)
               && ::CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                    &startup.StartupInfo, &process);
        if (startup.lpAttributeList) ::DeleteProcThreadAttributeList(startup.lpAttributeList);
        ::CloseHandle(in_rd); ::CloseHandle(out_wr); if (nul) ::CloseHandle(nul);
        if (!ok) { ::CloseHandle(in_wr); ::CloseHandle(out_rd); return faux; }
        ::CloseHandle(process.hThread);
        for (auto done = DWORD{}, left = (DWORD)script.size(); left; )
        {
            auto wrote = DWORD{};
            if (!::WriteFile(in_wr, script.data() + done, left, &wrote, nullptr) || !wrote) break;
            done += wrote; left -= wrote;
        }
        ::CloseHandle(in_wr);
        auto buf = std::array<char, 4096>{};
        for (auto n = DWORD{}; ::ReadFile(out_rd, buf.data(), (DWORD)buf.size(), &n, nullptr) && n; )
            out.append(buf.data(), n);
        ::CloseHandle(out_rd);
        ::WaitForSingleObject(process.hProcess, INFINITE);
        ::CloseHandle(process.hProcess);
    #endif
        auto error = faux;
        for (auto pos = size_t{}; pos < out.size(); )
        {
            auto eol = out.find('\n', pos);
            auto line = out.substr(pos, eol == text::npos ? text::npos : eol - pos);
            pos = eol == text::npos ? out.size() : eol + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if      (line[0] == '0') replies.push_back(line.substr(1));
            else if (line[0] == '2') error = true;
        }
        return !error;
    }

    inline auto settings_keyinfo(text const& path, text& comment, text& data) -> bool
    {
        auto replies = std::vector<text>{};
        auto ok = settings_pvputtygen_run("file " + path + "\nfingerprint\ncomment\n\n", replies);
        data = replies.size() > 1 ? replies[1] : text{};
        comment = replies.size() > 2 ? replies[2] : text{};
        auto recognized = !replies.empty() && (replies[0] == "ok" || replies[0] == "convertible");
        return recognized && ok && replies.size() >= 2;
    }

    inline auto make_connection_page(std::shared_ptr<settings_dialog_state> const& state,
                                     std::function<void()> accept,
                                     std::function<void()> close) -> component
    {
        auto numeric_input = [accept, close](std::function<text()> value,
                                             std::function<void(text)> change)
        {
            auto field = make_input({
                .value = std::move(value),
                .on_change = std::move(change),
                .on_submit = [accept](text){ accept(); },
                .on_cancel = [close]{ close(); },
                .digits_only = true,
                .palette = {
                    .background = theme::surface,
                    .foreground = theme::text_fg,
                    .muted_foreground = theme::subtext,
                    .focus = theme::sel_bg_act,
                },
            });
            field.widget->limits({ 6, 1 }, { 6, 1 });
            return field;
        };

        auto timeout_range_width = cell_width(settings_connection::timeout_range);
        auto timeout_fields = grid::ctor({
            .columns = {
                { .weight = 0, .minimum = cell_width(settings_connection::timeout_label),
                  .maximum = cell_width(settings_connection::timeout_label) },
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = 6, .maximum = 6 },
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = timeout_range_width,
                  .maximum = timeout_range_width },
            },
            .rows = { { .weight = 0, .minimum = 1, .maximum = 1 } },
            .handle_mode = grid_handle_mode::hidden,
        });
        timeout_fields->attach(make_settings_label(settings_connection::timeout_label), { .column = 0 });
        timeout_fields->attach(numeric_input(
            [state]{ return state->timeout; },
            [state](text value){ state->timeout = std::move(value); }), { .column = 2 });
        timeout_fields->attach(make_settings_label(settings_connection::timeout_range, label_role::hint),
                               { .column = 4 });

        auto timeout_scroll = make_scrollview({
            .content = { timeout_fields },
            .scroll_axes = axes::X_only,
        });
        auto timeout_content = flex::ctor({ .direction = flex_direction::column });
        timeout_content->attach(std::move(timeout_scroll));
        timeout_content->attach(make_settings_label(settings_connection::timeout_help,
                                                    label_role::hint, label_overflow::wrap));
        auto timeout_box = make_groupbox({
            .title = "Timeout",
            .content = { timeout_content },
        });

        auto reconnect_label_width = std::max(cell_width(settings_connection::retries_label),
                                              cell_width(settings_connection::delay_label));
        auto reconnect_range_width = std::max(cell_width(settings_connection::retries_range),
                                              cell_width(settings_connection::delay_range));
        auto reconnect_fields = grid::ctor({
            .columns = {
                { .weight = 0, .minimum = reconnect_label_width, .maximum = reconnect_label_width },
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = 6, .maximum = 6 },
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = reconnect_range_width,
                  .maximum = reconnect_range_width },
            },
            .rows = {
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = 1, .maximum = 1 },
            },
            .handle_mode = grid_handle_mode::hidden,
        });
        reconnect_fields->attach(make_settings_label(settings_connection::retries_label),
                                 { .column = 0, .row = 0 });
        reconnect_fields->attach(numeric_input(
            [state]{ return state->reconnect_count; },
            [state](text value){ state->reconnect_count = std::move(value); }),
            { .column = 2, .row = 0 });
        reconnect_fields->attach(make_settings_label(settings_connection::retries_range, label_role::hint),
                                 { .column = 4, .row = 0 });
        reconnect_fields->attach(make_settings_label(settings_connection::delay_label),
                                 { .column = 0, .row = 1 });
        reconnect_fields->attach(numeric_input(
            [state]{ return state->reconnect_delay; },
            [state](text value){ state->reconnect_delay = std::move(value); }),
            { .column = 2, .row = 1 });
        reconnect_fields->attach(make_settings_label(settings_connection::delay_range, label_role::hint),
                                 { .column = 4, .row = 1 });

        auto reconnect_scroll = make_scrollview({
            .content = { reconnect_fields },
            .scroll_axes = axes::X_only,
        });
        auto reconnect_content = flex::ctor({ .direction = flex_direction::column });
        reconnect_content->attach(std::move(reconnect_scroll));
        reconnect_content->attach(make_settings_label(settings_connection::reconnect_help,
                                                      label_role::hint,
                                                      label_overflow::wrap));
        auto reconnect_box = make_groupbox({
            .title = "Reconnection settings",
            .content = { reconnect_content },
        });

        auto page = flex::ctor({
            .direction = flex_direction::column,
            .row_gap = 1,
            .column_padding_width = 2,
            .row_padding_height = 0
        });
        page->attach(std::move(timeout_box), {
            .shrink = 0,
        });
        page->attach(std::move(reconnect_box), {
            .shrink = 0,
        });
        return { page };
    }

    inline auto settings_key_cell(settings_dialog_state const& state, si32 column, si32 row) -> text
    {
        if (row < 0 || row >= (si32)state.draft.keyfiles.size()) return {};
        if (column == 0) return fs::path{ state.draft.keyfiles[(size_t)row] }.filename().string();
        if (column == 1) return row < (si32)state.key_comments.size()
                             ? state.key_comments[(size_t)row] : text{};
        return row < (si32)state.key_data.size() ? state.key_data[(size_t)row] : text{};
    }

    inline auto settings_key_column_content_width(settings_dialog_state const& state,
                                                  si32 column) -> si32
    {
        auto width = si32{};
        for (auto row = si32{}; row < (si32)state.draft.keyfiles.size(); ++row)
            width = std::max(width, cell_width(settings_key_cell(state, column, row)));
        return width;
    }

    inline void settings_key_selection_changed(
        std::shared_ptr<settings_dialog_state> const& state)
    {
        if (auto remove = state->key_remove_button_wp.lock()) remove->base::deface();
    }

    inline void settings_store_key(std::shared_ptr<settings_dialog_state> const& state,
                                   text const& path, text const& comment, text const& data)
    {
        for (auto& existing : state->draft.keyfiles) if (existing == path) return;
        state->draft.keyfiles.push_back(path);
        state->key_comments.push_back(comment);
        state->key_data.push_back(data);
        state->key_selection = { (si32)state->draft.keyfiles.size() - 1 };
        ++state->key_table_revision;
        if (auto table = state->key_table_wp.lock()) table->base::deface();
        settings_key_selection_changed(state);
    }

    inline void settings_begin_key_conversion(std::shared_ptr<settings_dialog_state> const& state,
                                              text const& path, bool retry = faux);

    inline void settings_begin_key_conversion(std::shared_ptr<settings_dialog_state> const& state,
                                              text const& path, bool retry)
    {
        auto window = state->window_wp.lock();
        if (!window) return;
        auto base = fs::path{ path }.filename().string();
        auto prompt = "Enter the passphrase for \"" + base
                    + "\". The key will be converted to PuTTY (.ppk) format, protected with the same passphrase.";
        auto ec = std::error_code{};
        auto temp_dir = fs::temp_directory_path(ec);
        if (ec) temp_dir = fs::path{ "/tmp" };
        static std::atomic<uint64_t> sequence{};
        auto temp = (temp_dir / ("parvion-convert-"
                    + std::to_string(sequence.fetch_add(1)) + ".ppk")).string();
        window->base::attach(make_secret_dialog({
            .window_wp = state->window_wp,
            .focus_back_wp = state->popup_wp,
            .title = "Convert private key",
            .prompt = std::move(prompt),
            .on_cancel = []{},
            .is_retry = retry,
            .work = {
                .status = "Converting key...",
                .run = [path, temp](text const& pass, std::vector<text>& replies)
                {
                    return settings_pvputtygen_run("file " + path + "\npassword " + pass
                        + "\nwrite " + temp + "\nfingerprint\ncomment\n\n", replies);
                },
                .on_done = [state, path, temp](std::vector<text> replies, bool ok)
                {
                    if (!ok || replies.size() < 5)
                    {
                        auto error = std::error_code{};
                        fs::remove(fs::path{ temp }, error);
                        settings_begin_key_conversion(state, path, true);
                        return;
                    }
                    auto data = replies[3];
                    auto comment = replies[4];
                    auto default_dir = fs::path{ path }.parent_path().string();
                    auto default_name = fs::path{ path }.filename().replace_extension(".ppk").string();
                    auto picker = make_file_picker({
                        .window_wp = state->window_wp,
                        .focus_back_wp = state->popup_wp,
                        .mode = file_picker_mode::save,
                        .title = "Save converted key",
                        .initial_dir = std::move(default_dir),
                        .name = std::move(default_name),
                        .on_accept = [state, temp, comment, data](text const& chosen)
                        {
                            auto copy_error = std::error_code{};
                            fs::copy_file(fs::path{ temp }, fs::path{ chosen },
                                          fs::copy_options::overwrite_existing, copy_error);
                            auto remove_error = std::error_code{};
                            fs::remove(fs::path{ temp }, remove_error);
                            if (copy_error)
                            {
                                if (state->ctrl) state->ctrl->log_line(logtype::error,
                                    "Could not save converted key to " + chosen);
                                return;
                            }
                            settings_store_key(state, chosen, comment, data);
                        },
                        .on_cancel = [temp]
                        {
                            auto error = std::error_code{};
                            fs::remove(fs::path{ temp }, error);
                        },
                    });
                    if (auto window = state->window_wp.lock())
                        window->base::attach(picker.widget);
                },
            },
        }));
    }

    inline void settings_add_key(std::shared_ptr<settings_dialog_state> const& state,
                                 text const& path)
    {
        if (path.empty()) return;
        for (auto& existing : state->draft.keyfiles) if (existing == path) return;
        auto replies = std::vector<text>{};
        settings_pvputtygen_run("file " + path + "\nencrypted\n\n", replies);
        auto kind = replies.empty() ? text{} : replies[0];
        if (kind == "ok")
        {
            auto comment = text{};
            auto data = text{};
            settings_keyinfo(path, comment, data);
            settings_store_key(state, path, comment, data);
        }
        else if (kind == "convertible") settings_begin_key_conversion(state, path);
        else if (state->ctrl)
        {
            state->ctrl->log_line(logtype::error, kind == "incompatible"
                ? text{ "SSH1 keys are not supported (SSH2 only): " + path }
                : text{ "Could not load or parse private key: " + path });
        }
    }

    inline void settings_remove_keys(std::shared_ptr<settings_dialog_state> const& state)
    {
        if (state->key_selection.empty()) return;
        auto paths = std::vector<text>{};
        auto comments = std::vector<text>{};
        auto data = std::vector<text>{};
        for (auto row = si32{}; row < (si32)state->draft.keyfiles.size(); ++row)
        {
            if (state->key_selection.contains(row)) continue;
            paths.push_back(state->draft.keyfiles[(size_t)row]);
            comments.push_back(row < (si32)state->key_comments.size()
                             ? state->key_comments[(size_t)row] : text{});
            data.push_back(row < (si32)state->key_data.size()
                         ? state->key_data[(size_t)row] : text{});
        }
        state->draft.keyfiles = std::move(paths);
        state->key_comments = std::move(comments);
        state->key_data = std::move(data);
        state->key_selection.clear();
        ++state->key_table_revision;
        if (auto table = state->key_table_wp.lock()) table->base::deface();
        settings_key_selection_changed(state);
    }

    inline auto make_settings_key_table_cfg(std::shared_ptr<settings_dialog_state> const& state)
        -> table_cfg
    {
        auto cfg = table_cfg{};
        cfg.deletion.window_wp = state->window_wp;
        cfg.palette = table_palette{
            .bg = theme::bg,
            .header = theme::surface,
            .text_fg = theme::text_fg,
            .subtext = theme::subtext,
            .sel_bg = theme::sel_bg,
            .sel_bg_act = theme::sel_bg_act,
            .sort_fg = theme::sort_fg,
            .sb_track = theme::sb_track,
            .sb_thumb = theme::sb_thumb,
            .sb_hover = theme::sb_hover,
            .sb_drag = theme::sb_drag,
        };
        cfg.columns = [state]
        {
            auto table = qtable{};
            for (auto column = si32{}; column < (si32)settings_sftp::key_headers.size(); ++column)
            {
                table.add_column(qtable::column{
                    .title = text{ settings_sftp::key_headers[(size_t)column] },
                    .width = state->key_column_widths[(size_t)column],
                    .right = faux,
                    .resizable = true,
                    .key = column,
                }, state->key_columns_shown[(size_t)column],
                   text{ settings_sftp::key_menu_headers[(size_t)column] });
            }
            table.on_show_column = [state](si32 column, bool shown)
            {
                if (column >= 0 && column < (si32)settings_sftp::key_headers.size())
                    state->key_columns_shown[(size_t)column] = shown;
            };
            table.on_resize_column = [state](si32 column, si32 width)
            {
                if (column >= 0 && column < (si32)settings_sftp::key_headers.size())
                    state->key_column_widths[(size_t)column] = width;
            };
            table.autofit = [state](si32 column)
            {
                return settings_key_column_content_width(*state, column);
            };
            return table;
        };
        cfg.row_count = [state]{ return (si32)state->draft.keyfiles.size(); };
        cfg.viewport.behavior = [state]
        {
            return table_viewport_refresh{ state->key_table_revision };
        };
        cfg.cell = [state](si32 row, si32 column)
        {
            return table_cell{ settings_key_cell(*state, column, row), theme::text_fg };
        };
        cfg.sort.compare = [state](si32 left, si32 right, si32 column)
        {
            auto lhs = settings_key_cell(*state, column, left); utf::to_lower(lhs);
            auto rhs = settings_key_cell(*state, column, right); utf::to_lower(rhs);
            return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
        };
        cfg.selection = [state]
        {
            auto selection = qsel_cfg{};
            selection.key_count = [state]{ return (si32)state->draft.keyfiles.size(); };
            selection.is_selected = [state](si32 key){ return state->key_selection.contains(key); };
            selection.on_select = [state](si32 key, bool selected)
            {
                if (key < 0 || key >= (si32)state->draft.keyfiles.size()) return;
                if (selected) state->key_selection.insert(key);
                else          state->key_selection.erase(key);
                settings_key_selection_changed(state);
            };
            selection.on_clear = [state]
            {
                state->key_selection.clear();
                settings_key_selection_changed(state);
            };
            selection.has_selection = [state]{ return !state->key_selection.empty(); };
            selection.in_scope = [state](si32 key)
            {
                return key >= 0 && key < (si32)state->draft.keyfiles.size();
            };
            selection.row_count = [state]{ return (si32)state->draft.keyfiles.size(); };
            selection.key_of_row = [state](si32 row)
            {
                return row >= 0 && row < (si32)state->draft.keyfiles.size() ? row : -1;
            };
            return selection;
        };
        cfg.deletion.enabled = true;
        cfg.deletion.on_remove_selected = [state](netxs::wptr<ui::base>)
        {
            settings_remove_keys(state);
        };
        cfg.empty_text = []{ return text{ "No private keys configured." }; };
        cfg.behavior.wide_hit = true;
        return cfg;
    }

    inline void settings_open_key_picker(std::shared_ptr<settings_dialog_state> const& state,
                                         id_t gear_id = {})
    {
        auto picker = make_file_picker({
            .window_wp = state->window_wp,
            .focus_back_wp = state->popup_wp,
            .gear_id = gear_id,
            .mode = file_picker_mode::open,
            .selection = file_picker_selection::files,
            .title = "Add key file",
            .initial_dir = user_home_dir(),
            .on_accept = [state](text const& path){ settings_add_key(state, path); },
        });
        if (auto window = state->window_wp.lock())
            window->base::attach(picker.widget);
    }

    inline auto make_sftp_page(std::shared_ptr<settings_dialog_state> const& state,
                               std::function<void()> accept,
                               std::function<void()> close) -> component
    {
        auto numeric_input = [accept, close](std::function<text()> value,
                                             std::function<void(text)> change)
        {
            auto field = make_input({
                .value = std::move(value),
                .on_change = std::move(change),
                .on_submit = [accept](text){ accept(); },
                .on_cancel = [close]{ close(); },
                .digits_only = true,
                .palette = {
                    .background = theme::surface,
                    .foreground = theme::text_fg,
                    .muted_foreground = theme::subtext,
                    .focus = theme::sel_bg_act,
                },
            });
            field.widget->limits({ 6, 1 }, { 6, 1 });
            return field;
        };

        auto key_table = make_table(make_settings_key_table_cfg(state));
        key_table.widget->limits({ 24, 5 }, { -1, 8 });
        state->key_table_wp = ptr::shadow(key_table.widget);
        auto key_buttons = flex::ctor({
            .direction = flex_direction::row,
            .align_items = flex_align::stretch,
            .column_gap = 1,
        });
        key_buttons->limits({ 0, 1 }, { -1, 1 });
        key_buttons->attach(make_button({
            .label = []{ return text{ " Add key file... " }; },
            .on_activate = [state](hids& gear, ui::base&)
            {
                settings_open_key_picker(state, gear.id);
            },
        }), { .basis = 17, .minimum = 17, .maximum = 17 });
        auto remove_key = make_button({
            .label = []{ return text{ " Remove key " }; },
            .on_activate = [state](hids&, ui::base&){ settings_remove_keys(state); },
            .enabled = [state]{ return !state->key_selection.empty(); },
        });
        state->key_remove_button_wp = ptr::shadow(remove_key.widget);
        key_buttons->attach(std::move(remove_key),
                            { .basis = 12, .minimum = 12, .maximum = 12 });

        auto public_key_content = flex::ctor({
            .direction = flex_direction::column,
            .row_gap = 0,
        });
        public_key_content->attach(make_settings_label(settings_sftp::public_key_help,
                                                        label_role::hint, label_overflow::wrap),
                                   { .shrink = 0 });
        public_key_content->attach(make_settings_label(settings_sftp::private_keys_label),
                                   { .shrink = 0 });
        public_key_content->attach(std::move(key_table),
                                   { .grow = 1, .shrink = 0, .basis = 5, .minimum = 5, .maximum = 8 });
        public_key_content->attach_separator(1);
        public_key_content->attach(component{ key_buttons }, { .shrink = 0 });
        auto public_key_box = make_groupbox({
            .title = "Public Key Authentication",
            .content = { public_key_content },
        });

        auto hash_options = std::vector<dropdown_option>{
            { text{ settings_sftp::hash_none } },
        };
        for (auto algorithm = si32{}; algorithm < hash_algo_count; ++algorithm)
            hash_options.push_back({ text{ hash_algo_label(algorithm) } });
        auto hash_dropdown = make_dropdown({
            .options = std::move(hash_options),
            .selected = [state]{ return state->hash_on_transfer ? state->hash_algo + 1 : 0; },
            .on_change = [state](si32 selected)
            {
                state->hash_on_transfer = selected > 0;
                if (selected > 0) state->hash_algo = selected - 1;
            },
        });
        auto hash_width = hash_dropdown.widget->base::min_sz.x;
        auto hash_fields = grid::ctor({
            .columns = {
                { .weight = 0, .minimum = cell_width(settings_sftp::hash_label),
                  .maximum = cell_width(settings_sftp::hash_label) },
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = hash_width, .maximum = hash_width },
                { .weight = 1 },
            },
            .rows = { { .weight = 0, .minimum = 1, .maximum = 1 } },
            .handle_mode = grid_handle_mode::hidden,
        });
        hash_fields->attach(make_settings_label(settings_sftp::hash_label), { .column = 0 });
        hash_fields->attach(std::move(hash_dropdown), { .column = 2 });
        auto hash_scroll = make_scrollview({
            .content = { hash_fields },
            .scroll_axes = axes::X_only,
        });
        auto hash_box = make_groupbox({
            .title = "Hash verification",
            .content = std::move(hash_scroll),
        });

        auto compression_scroll = make_scrollview({
            .content = make_checkbox({
                .label = []{ return text{ "Enable compression" }; },
                .checked = [state]{ return state->compression; },
                .on_change = [state](bool checked){ state->compression = checked; },
            }),
            .scroll_axes = axes::X_only,
        });
        auto compression_box = make_groupbox({
            .title = "Other SFTP options",
            .content = std::move(compression_scroll),
        });

        auto unit_options = std::vector<dropdown_option>{};
        for (auto unit = si32{}; unit < sftp_unit_count; ++unit)
            unit_options.push_back({ text{ sftp_unit_label(unit) } });
        auto unit_dropdown = make_dropdown({
            .options = std::move(unit_options),
            .selected = [state]{ return state->threshold_unit; },
            .on_change = [state](si32 selected){ state->threshold_unit = selected; },
        });
        auto unit_width = unit_dropdown.widget->base::min_sz.x;

        auto allocation_options = std::vector<dropdown_option>{};
        for (auto policy = si32{}; policy < allocation_count; ++policy)
            allocation_options.push_back({ text{ transfer_allocation_label(policy) } });
        auto allocation_dropdown = make_dropdown({
            .options = std::move(allocation_options),
            .selected = [state]{ return state->transfer_allocation; },
            .on_change = [state](si32 selected){ state->transfer_allocation = selected; },
        });
        auto allocation_width = allocation_dropdown.widget->base::min_sz.x;

        auto conflict_options = std::vector<dropdown_option>{};
        for (auto policy = si32{}; policy < conflict_policy_count; ++policy)
            conflict_options.push_back({ text{ conflict_policy_label(policy) } });
        auto conflict_dropdown = make_dropdown({
            .options = std::move(conflict_options),
            .selected = [state]{ return (si32)state->conflict_policy; },
            .on_change = [state](si32 selected)
            {
                state->conflict_policy = (conflict_policy_t)selected;
            },
        });
        auto conflict_width = conflict_dropdown.widget->base::min_sz.x;

        auto threshold_controls = flex::ctor({
            .direction = flex_direction::row,
            .align_items = flex_align::stretch,
            .column_gap = 1,
        });
        threshold_controls->attach(numeric_input(
            [state]{ return state->threshold_value; },
            [state](text value){ state->threshold_value = std::move(value); }),
            { .basis = 6, .minimum = 6, .maximum = 6 });
        threshold_controls->attach(std::move(unit_dropdown),
            { .basis = unit_width, .minimum = unit_width, .maximum = unit_width });

        auto connection_controls = flex::ctor({
            .direction = flex_direction::row,
            .align_items = flex_align::stretch,
            .column_gap = 1,
        });
        connection_controls->attach(numeric_input(
            [state]{ return state->max_connections; },
            [state](text value){ state->max_connections = std::move(value); }),
            { .basis = 6, .minimum = 6, .maximum = 6 });
        connection_controls->attach(make_settings_label(settings_sftp::max_connections_hint,
                                                         label_role::hint),
            { .shrink = 0 });

        auto label_width = std::max({ cell_width(settings_sftp::threshold_label),
                                      cell_width(settings_sftp::max_connections_label),
                                      cell_width(settings_sftp::allocation_label) });
        auto control_width = std::max({ 6 + 2 + unit_width,
                                        6 + 2 + cell_width(settings_sftp::max_connections_hint),
                                        allocation_width });
        auto parallel_fields = grid::ctor({
            .columns = {
                { .weight = 0, .minimum = label_width, .maximum = label_width },
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = control_width, .maximum = control_width },
                { .weight = 1 },
            },
            .rows = {
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = 1, .maximum = 1 },
            },
            .handle_mode = grid_handle_mode::hidden,
        });
        parallel_fields->attach(make_settings_label(settings_sftp::threshold_label),
                                { .column = 0, .row = 0 });
        parallel_fields->attach(component{ threshold_controls }, { .column = 2, .row = 0 });
        parallel_fields->attach(make_settings_label(settings_sftp::max_connections_label),
                                { .column = 0, .row = 1 });
        parallel_fields->attach(component{ connection_controls }, { .column = 2, .row = 1 });
        parallel_fields->attach(make_settings_label(settings_sftp::allocation_label),
                                { .column = 0, .row = 2 });
        parallel_fields->attach(std::move(allocation_dropdown), { .column = 2, .row = 2 });
        auto parallel_scroll = make_scrollview({
            .content = { parallel_fields },
            .scroll_axes = axes::X_only,
        });
        auto parallel_box = make_groupbox({
            .title = "Parallel transfers",
            .content = std::move(parallel_scroll),
        });

        auto conflict_fields = grid::ctor({
            .columns = {
                { .weight = 0, .minimum = cell_width(settings_sftp::existing_policy_label),
                  .maximum = cell_width(settings_sftp::existing_policy_label) },
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = conflict_width, .maximum = conflict_width },
                { .weight = 1 },
            },
            .rows = { { .weight = 0, .minimum = 1, .maximum = 1 } },
            .handle_mode = grid_handle_mode::hidden,
        });
        conflict_fields->attach(make_settings_label(settings_sftp::existing_policy_label),
                                { .column = 0 });
        conflict_fields->attach(std::move(conflict_dropdown), { .column = 2 });
        auto conflict_scroll = make_scrollview({
            .content = { conflict_fields },
            .scroll_axes = axes::X_only,
        });
        auto conflict_box = make_groupbox({
            .title = "Conflict handling",
            .content = std::move(conflict_scroll),
        });

        auto page = flex::ctor({
            .direction = flex_direction::column,
            .row_gap = 1,
            .column_padding_width = 2,
            .row_padding_height = 0
        });
        page->attach(std::move(public_key_box), { .shrink = 0 });
        page->attach(std::move(hash_box), { .shrink = 0 });
        page->attach(std::move(compression_box), { .shrink = 0 });
        page->attach(std::move(parallel_box), { .shrink = 0 });
        page->attach(std::move(conflict_box), { .shrink = 0 });
        return { page };
    }

    inline auto settings_site_cell(settings_dialog_state const& state,
                                   si32 column, si32 row) -> text
    {
        if (row < 0 || row >= (si32)state.draft.sites.size()) return {};
        auto const& site = state.draft.sites[(size_t)row];
        if (column == 0) return site.name;
        if (column == 1) return site.host;
        if (column == 2) return std::to_string(site.port);
        if (column == 3) return site.user;
        return {};
    }

    inline auto settings_site_column_content_width(settings_dialog_state const& state,
                                                    si32 column) -> si32
    {
        auto width = si32{};
        for (auto row = si32{}; row < (si32)state.draft.sites.size(); ++row)
            width = std::max(width, cell_width(settings_site_cell(state, column, row)));
        return width;
    }

    inline auto settings_single_site(settings_dialog_state const& state) -> si32
    {
        if (state.site_selection.size() != 1) return -1;
        auto selected = *state.site_selection.begin();
        return selected >= 0 && selected < (si32)state.draft.sites.size() ? selected : -1;
    }

    inline void settings_site_selection_changed(
        std::shared_ptr<settings_dialog_state> const& state)
    {
        if (auto edit = state->site_edit_button_wp.lock()) edit->base::deface();
        if (auto remove = state->site_remove_button_wp.lock()) remove->base::deface();
    }

    inline void settings_remove_sites(std::shared_ptr<settings_dialog_state> const& state)
    {
        if (state->site_selection.empty()) return;
        auto kept = std::vector<saved_site>{};
        kept.reserve(state->draft.sites.size());
        for (auto row = si32{}; row < (si32)state->draft.sites.size(); ++row)
            if (!state->site_selection.contains(row))
                kept.push_back(std::move(state->draft.sites[(size_t)row]));
        state->draft.sites = std::move(kept);
        state->site_selection.clear();
        ++state->site_table_revision;
        if (auto table = state->site_table_wp.lock()) table->base::deface();
        settings_site_selection_changed(state);
    }

    struct settings_site_editor_state
    {
        std::shared_ptr<settings_dialog_state> parent;
        si32 edit_index = -1;
        std::array<text, settings_site::field_count> value{};
        text error;
        bool done = faux;
        netxs::wptr<ui::base> popup_wp;
        netxs::wptr<ui::base> error_wp;
        netxs::wptr<ui::base> error_separator_wp;
    };

    inline auto make_settings_site_editor(
        std::shared_ptr<settings_dialog_state> const& parent,
        si32 edit_index) -> ui::sptr
    {
        auto state = std::make_shared<settings_site_editor_state>();
        state->parent = parent;
        state->edit_index = edit_index;
        if (edit_index >= 0 && edit_index < (si32)parent->draft.sites.size())
        {
            auto const& site = parent->draft.sites[(size_t)edit_index];
            state->value = { site.name, site.host, std::to_string(site.port), site.user, site.pass };
        }
        else state->value[settings_site::port] = "22";

        auto popup_ref = std::make_shared<netxs::wptr<ui::base>>();
        auto gear_id = id_t{};
        if (auto window = parent->window_wp.lock())
            gear_id = window->bell::indexer.luafx.get_gear().id;
        auto finish = [window_wp = parent->window_wp,
                       focus_back_wp = parent->popup_wp,
                       popup_ref,
                       gear_id]
        {
            if (auto window = window_wp.lock())
            {
                window->base::enqueue([focus_back_wp, popup_ref, gear_id](auto&)
                {
                    if (auto popup = popup_ref->lock()) popup->base::detach();
                    if (auto dialog = focus_back_wp.lock())
                    {
                        pro::focus::set(dialog, gear_id, solo::on);
                        dialog->base::deface();
                    }
                });
            }
        };

        auto show_error = [state](text error)
        {
            state->error = std::move(error);
            if (auto label = state->error_wp.lock())
            {
                label->base::hidden = faux;
                label->base::deface();
            }
            if (auto separator = state->error_separator_wp.lock())
                separator->base::hidden = faux;
            if (auto popup = state->popup_wp.lock())
            {
                popup->base::deface();
                popup->base::reflow();
            }
        };
        auto submit = [state, show_error, finish]
        {
            if (state->done || !state->parent) return;
            auto& parent_state = *state->parent;
            auto name = state->value[settings_site::name];
            auto host = state->value[settings_site::host];
            if (host.empty())
            {
                show_error("Host is required.");
                return;
            }
            if (!valid_site_host(host))
            {
                show_error("Host must be a valid hostname, IPv4, or IPv6 address.");
                return;
            }
            auto port = si32{ 22 };
            auto const& port_text = state->value[settings_site::port];
            if (!port_text.empty())
            {
                auto parsed = si64{};
                for (auto c : port_text)
                {
                    if (c < '0' || c > '9')
                    {
                        show_error("Port must be a number from 1 to 65535.");
                        return;
                    }
                    parsed = parsed * 10 + (c - '0');
                    if (parsed > 65535) break;
                }
                if (parsed < 1 || parsed > 65535)
                {
                    show_error("Port must be a number from 1 to 65535.");
                    return;
                }
                port = (si32)parsed;
            }

            if (name.empty())
                name = state->value[settings_site::user] + "@" + host
                     + ":" + std::to_string(port);
            for (auto row = si32{}; row < (si32)parent_state.draft.sites.size(); ++row)
                if (row != state->edit_index
                 && parent_state.draft.sites[(size_t)row].name == name)
                {
                    show_error("Site Name must be unique.");
                    return;
                }

            auto site = saved_site{
                std::move(name),
                std::move(host),
                port,
                state->value[settings_site::user],
                state->value[settings_site::password],
            };
            if (state->edit_index >= 0
             && state->edit_index < (si32)parent_state.draft.sites.size())
            {
                parent_state.draft.sites[(size_t)state->edit_index] = std::move(site);
                parent_state.site_selection = { state->edit_index };
            }
            else
            {
                parent_state.draft.sites.push_back(std::move(site));
                parent_state.site_selection = {
                    (si32)parent_state.draft.sites.size() - 1
                };
            }
            ++parent_state.site_table_revision;
            if (auto table = parent_state.site_table_wp.lock()) table->base::deface();
            settings_site_selection_changed(state->parent);
            state->done = true;
            finish();
        };
        auto cancel = [state, finish]
        {
            if (std::exchange(state->done, true)) return;
            finish();
        };

        auto label_width = si32{};
        for (auto label : settings_site::field_labels)
            label_width = std::max(label_width, cell_width(label));
        auto fields = grid::ctor({
            .columns = {
                { .weight = 0, .minimum = label_width, .maximum = label_width },
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 1, .minimum = 1 },
            },
            .rows = std::vector<grid_track>((size_t)settings_site::field_count,
                { .weight = 0, .minimum = 1, .maximum = 1 }),
            .handle_mode = grid_handle_mode::hidden,
        });
        auto first_input = netxs::wptr<ui::base>{};
        for (auto field = si32{}; field < settings_site::field_count; ++field)
        {
            fields->attach(make_settings_label(settings_site::field_labels[(size_t)field]),
                           { .column = 0, .row = field });
            auto input = make_input({
                .value = [state, field]{ return state->value[(size_t)field]; },
                .on_change = [state, field](text value)
                {
                    state->value[(size_t)field] = std::move(value);
                    if (!state->error.empty())
                    {
                        state->error.clear();
                        if (auto label = state->error_wp.lock())
                        {
                            label->base::hidden = true;
                            label->base::deface();
                        }
                        if (auto separator = state->error_separator_wp.lock())
                            separator->base::hidden = true;
                        if (auto popup = state->popup_wp.lock())
                        {
                            popup->base::deface();
                            popup->base::reflow();
                        }
                    }
                },
                .on_submit = [submit](text){ submit(); },
                .on_cancel = cancel,
                .secret = field == settings_site::password,
                .digits_only = field == settings_site::port,
                .focus_on_start = field == settings_site::name,
                .palette = {
                    .background = theme::bg,
                    .foreground = theme::text_fg,
                    .muted_foreground = theme::subtext,
                    .focus = theme::sel_bg_act,
                },
            });
            if (field == settings_site::name) first_input = ptr::shadow(input.widget);
            fields->attach(std::move(input), { .column = 2, .row = field });
        }

        auto content = flex::ctor({
            .direction = flex_direction::column,
            .column_padding_width = 2,
            .row_padding_height = 1,
        });
        content->attach(component{ fields }, {
            .shrink = 0,
            .basis = settings_site::field_count,
            .minimum = settings_site::field_count,
            .maximum = settings_site::field_count,
        });
        auto error_separator = content->attach_separator(1);
        error_separator->base::hidden = true;
        state->error_separator_wp = ptr::shadow(error_separator);
        auto error = make_label({
            .value = [state]{ return state->error; },
            .palette = { .text = theme::err_fg },
        });
        error.widget->base::hidden = true;
        state->error_wp = ptr::shadow(error.widget);
        content->attach(std::move(error), {
            .shrink = 0,
            .basis = 1,
            .minimum = 1,
            .maximum = 1,
        });

        auto buttons = flex::ctor({
            .direction = flex_direction::row,
            .justify_content = flex_justify::end,
            .align_items = flex_align::stretch,
            .column_gap = 1,
        });
        buttons->attach(make_button({
            .label = []{ return text{ " OK " }; },
            .on_activate = [submit](hids&, ui::base&){ submit(); },
        }), { .basis = 6, .minimum = 6, .maximum = 6 });
        buttons->attach(make_button({
            .label = []{ return text{ " Cancel " }; },
            .on_activate = [cancel](hids&, ui::base&){ cancel(); },
        }), { .basis = 10, .minimum = 10, .maximum = 10 });

        auto popup = make_dialog({
            .title = make_label({
                .value = [title = text{ edit_index < 0 ? "Add SFTP Site"
                                                       : "Edit SFTP Site" }]
                {
                    return title;
                },
                .palette = {
                    .text = theme::title_fg_act,
                    .background = theme::header,
                },
            }),
            .content = { content },
            .buttons = { buttons },
            .position = dialog_anchor::center,
            .size = {
                .width = dialog_length::cells(64),
            },
            .minimum = { 52, 3 },
            .maximum = { 72, -1 },
            .fit_content_height = true,
            .on_cancel = cancel,
        });
        *popup_ref = ptr::shadow(popup.widget);
        state->popup_wp = *popup_ref;

        if (auto window = parent->window_wp.lock())
        {
            window->base::enqueue([first_input, gear_id](auto&)
            {
                if (auto input = first_input.lock()) pro::focus::set(input, gear_id, solo::on);
            });
        }
        return popup.widget;
    }

    inline void settings_open_site_editor(
        std::shared_ptr<settings_dialog_state> const& state,
        si32 edit_index)
    {
        auto window = state->window_wp.lock();
        if (!window || edit_index >= (si32)state->draft.sites.size()) return;
        window->base::attach(make_settings_site_editor(state, edit_index));
    }

    inline auto make_settings_site_table_cfg(
        std::shared_ptr<settings_dialog_state> const& state,
        std::function<void()> close) -> table_cfg
    {
        auto cfg = table_cfg{};
        cfg.deletion.window_wp = state->window_wp;
        cfg.palette = table_palette{
            .bg = theme::bg,
            .header = theme::surface,
            .text_fg = theme::text_fg,
            .subtext = theme::subtext,
            .sel_bg = theme::sel_bg,
            .sel_bg_act = theme::sel_bg_act,
            .sort_fg = theme::sort_fg,
            .sb_track = theme::sb_track,
            .sb_thumb = theme::sb_thumb,
            .sb_hover = theme::sb_hover,
            .sb_drag = theme::sb_drag,
        };
        cfg.columns = [state]
        {
            auto table = qtable{};
            for (auto column = si32{}; column < (si32)settings_site::headers.size(); ++column)
            {
                table.add_column(qtable::column{
                    .title = text{ settings_site::headers[(size_t)column] },
                    .width = state->site_column_widths[(size_t)column],
                    .right = column == 2,
                    .resizable = true,
                    .key = column,
                }, state->site_columns_shown[(size_t)column],
                   text{ settings_site::menu_headers[(size_t)column] });
            }
            table.on_show_column = [state](si32 column, bool shown)
            {
                if (column >= 0 && column < (si32)settings_site::headers.size())
                    state->site_columns_shown[(size_t)column] = shown;
            };
            table.on_resize_column = [state](si32 column, si32 width)
            {
                if (column >= 0 && column < (si32)settings_site::headers.size())
                    state->site_column_widths[(size_t)column] = width;
            };
            table.autofit = [state](si32 column)
            {
                return column >= 0 && column < (si32)settings_site::headers.size()
                     ? settings_site_column_content_width(*state, column)
                     : si32{};
            };
            return table;
        };
        cfg.row_count = [state]{ return (si32)state->draft.sites.size(); };
        cfg.viewport.behavior = [state]
        {
            return table_viewport_refresh{ state->site_table_revision };
        };
        cfg.cell = [state](si32 row, si32 column)
        {
            return table_cell{ settings_site_cell(*state, column, row), theme::text_fg };
        };
        cfg.sort.compare = [state](si32 left, si32 right, si32 column)
        {
            if (column < 0 || column >= (si32)settings_site::headers.size()) return si32{};
            if (column == 2)
            {
                auto lhs = state->draft.sites[(size_t)left].port;
                auto rhs = state->draft.sites[(size_t)right].port;
                return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
            }
            auto lhs = settings_site_cell(*state, column, left); utf::to_lower(lhs);
            auto rhs = settings_site_cell(*state, column, right); utf::to_lower(rhs);
            return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
        };
        cfg.selection = [state]
        {
            auto selection = qsel_cfg{};
            selection.key_count = [state]{ return (si32)state->draft.sites.size(); };
            selection.is_selected = [state](si32 key)
            {
                return state->site_selection.contains(key);
            };
            selection.on_select = [state](si32 key, bool selected)
            {
                if (key < 0 || key >= (si32)state->draft.sites.size()) return;
                if (selected) state->site_selection.insert(key);
                else          state->site_selection.erase(key);
                settings_site_selection_changed(state);
            };
            selection.on_clear = [state]
            {
                state->site_selection.clear();
                settings_site_selection_changed(state);
            };
            selection.has_selection = [state]{ return !state->site_selection.empty(); };
            selection.in_scope = [state](si32 key)
            {
                return key >= 0 && key < (si32)state->draft.sites.size();
            };
            selection.row_count = [state]{ return (si32)state->draft.sites.size(); };
            selection.key_of_row = [state](si32 row)
            {
                return row >= 0 && row < (si32)state->draft.sites.size() ? row : -1;
            };
            return selection;
        };
        cfg.on_activate = [state](si32 row)
        {
            if (settings_single_site(*state) == row) settings_open_site_editor(state, row);
        };
        cfg.on_key = [close = std::move(close)](hids& gear, netxs::wptr<ui::base>)
        {
            if (gear.keybd::generic() == input::key::Esc)
            {
                gear.set_handled();
                close();
                return table_viewport_action{ table_viewport_action::handled };
            }
            return table_viewport_action{};
        };
        cfg.deletion.enabled = true;
        cfg.deletion.on_remove_selected = [state](netxs::wptr<ui::base>)
        {
            settings_remove_sites(state);
        };
        cfg.empty_text = []{ return text{ "No sites configured." }; };
        cfg.behavior.wide_hit = true;
        return cfg;
    }

    inline auto make_site_page(std::shared_ptr<settings_dialog_state> const& state,
                               std::function<void()> close) -> component
    {
        auto table = make_table(make_settings_site_table_cfg(state, std::move(close)));
        table.widget->limits({ 24, 5 }, { -1, -1 });
        state->site_table_wp = ptr::shadow(table.widget);

        auto buttons = flex::ctor({
            .direction = flex_direction::row,
            .align_items = flex_align::stretch,
            .column_gap = 1,
        });
        buttons->limits({ 0, 1 }, { -1, 1 });
        buttons->attach(make_button({
            .label = []{ return text{ " Add " }; },
            .on_activate = [state](hids&, ui::base&)
            {
                settings_open_site_editor(state, -1);
            },
        }), { .basis = 5, .minimum = 5, .maximum = 5 });
        auto edit = make_button({
            .label = []{ return text{ " Edit " }; },
            .on_activate = [state](hids&, ui::base&)
            {
                if (auto selected = settings_single_site(*state); selected >= 0)
                    settings_open_site_editor(state, selected);
            },
            .enabled = [state]{ return settings_single_site(*state) >= 0; },
        });
        state->site_edit_button_wp = ptr::shadow(edit.widget);
        buttons->attach(std::move(edit), { .basis = 6, .minimum = 6, .maximum = 6 });
        auto remove = make_button({
            .label = []{ return text{ " Remove " }; },
            .on_activate = [state](hids&, ui::base&){ settings_remove_sites(state); },
            .enabled = [state]{ return !state->site_selection.empty(); },
        });
        state->site_remove_button_wp = ptr::shadow(remove.widget);
        buttons->attach(std::move(remove), { .basis = 8, .minimum = 8, .maximum = 8 });

        auto content = flex::ctor({
            .direction = flex_direction::column,
        });
        content->attach(make_settings_label(settings_site::help, label_role::hint, label_overflow::wrap),
                        { .shrink = 0 });
        content->attach(make_settings_label(settings_site::sites_label), { .shrink = 0 });
        content->attach(std::move(table),
                        { .grow = 1, .shrink = 0, .basis = 5, .minimum = 5 });
        content->attach_separator(1);
        content->attach(component{ buttons }, { .shrink = 0 });
        auto manager = make_groupbox({
            .title = "Site Manager",
            .content = { content },
        });

        auto page = flex::ctor({
            .direction = flex_direction::column,
            .row_gap = 1,
            .column_padding_width = 2,
            .row_padding_height = 0
        });
        page->attach(std::move(manager), { .grow = 1, .shrink = 0 });
        return { page };
    }

    inline auto make_debug_page(std::shared_ptr<settings_dialog_state> const& state) -> component
    {
        auto levels = std::vector<dropdown_option>{};
        for (auto level = si32{ log_debug_none }; level <= log_debug_debug; ++level)
            levels.push_back({ std::to_string(level) + " - " + text{ log_debug_label(level) } });
        auto level_dropdown = make_dropdown({
            .options = std::move(levels),
            .selected = [state]{ return state->log_debug_level; },
            .on_change = [state](si32 selected){ state->log_debug_level = selected; },
        });
        auto level_width = level_dropdown.widget->base::min_sz.x;
        auto level_fields = grid::ctor({
            .columns = {
                { .weight = 0, .minimum = cell_width(settings_debug::level_label),
                  .maximum = cell_width(settings_debug::level_label) },
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 0, .minimum = level_width, .maximum = level_width },
                { .weight = 1 },
            },
            .rows = { { .weight = 0, .minimum = 1, .maximum = 1 } },
            .handle_mode = grid_handle_mode::hidden,
        });
        level_fields->attach(make_settings_label(settings_debug::level_label), { .column = 0 });
        level_fields->attach(std::move(level_dropdown), { .column = 2 });

        auto level_scroll = make_scrollview({
            .content = { level_fields },
            .scroll_axes = axes::X_only,
        });
        auto debugging_content = flex::ctor({ .direction = flex_direction::column });
        debugging_content->attach(std::move(level_scroll), { .shrink = 0 });
        debugging_content->attach(make_settings_label(settings_debug::help, label_role::hint, label_overflow::wrap),
                                  { .shrink = 0 });
        auto debugging = make_groupbox({
            .title = "Debugging settings",
            .content = { debugging_content },
        });
        auto listing_scroll = make_scrollview({
            .content = make_checkbox({
                .label = []{ return text{ "Show raw directory listing" }; },
                .checked = [state]{ return state->log_raw_listing; },
                .on_change = [state](bool checked){ state->log_raw_listing = checked; },
            }),
            .scroll_axes = axes::X_only,
        });
        auto listing = make_groupbox({
            .title = "Directory listing",
            .content = std::move(listing_scroll),
        });

        auto page = flex::ctor({
            .direction = flex_direction::column,
            .row_gap = 1,
            .column_padding_width = 2,
            .row_padding_height = 0
        });
        page->attach(std::move(debugging), { .shrink = 0 });
        page->attach(std::move(listing), { .shrink = 0 });
        return { page };
    }

    inline auto make_settings_dialog(sftp_remote* ctrl,
                                     netxs::wptr<ui::base> window_wp,
                                     ui::sptr* card_out = nullptr) -> ui::sptr
    {
        auto state = std::make_shared<settings_dialog_state>();
        state->ctrl = ctrl;
        state->window_wp = window_wp;
        if (ctrl) state->draft = ctrl->cfg;
        state->timeout = std::to_string(state->draft.timeout);
        state->reconnect_count = std::to_string(state->draft.reconnect_count);
        state->reconnect_delay = std::to_string(state->draft.reconnect_delay);
        state->threshold_value = std::to_string(state->draft.threshold_value);
        state->max_connections = std::to_string(state->draft.max_connections);
        state->compression = state->draft.compression;
        state->threshold_unit = state->draft.threshold_unit;
        state->transfer_allocation = state->draft.transfer_allocation;
        state->conflict_policy = state->draft.conflict_policy;
        state->hash_on_transfer = state->draft.hash_on_transfer;
        state->hash_algo = state->draft.hash_algo;
        state->log_debug_level = state->draft.log_debug_level;
        state->log_raw_listing = state->draft.log_raw_listing;
        state->key_comments.assign(state->draft.keyfiles.size(), text{});
        state->key_data.assign(state->draft.keyfiles.size(), text{});
        for (auto index = size_t{}; index < state->draft.keyfiles.size(); ++index)
        {
            settings_keyinfo(state->draft.keyfiles[index],
                             state->key_comments[index], state->key_data[index]);
        }

        auto clear_guard = [state]
        {
            if (auto window = state->window_wp.lock())
                window->base::property("parvion.settings.active", faux) = faux;
        };
        auto close = [state, clear_guard]
        {
            if (std::exchange(state->done, true)) return;
            if (auto dropdown = active_dropdown_popup()) dismiss_dropdown(dropdown);
            clear_guard();
            if (auto popup = state->popup_wp.lock()) popup->base::detach();
        };
        auto accept = [state, clear_guard]
        {
            if (std::exchange(state->done, true)) return;
            auto parse = [](text const& value)
            {
                return value.empty() ? 0 : std::atoi(value.c_str());
            };
            state->draft.timeout = parse(state->timeout);
            state->draft.reconnect_count = parse(state->reconnect_count);
            state->draft.reconnect_delay = parse(state->reconnect_delay);
            state->draft.threshold_value = parse(state->threshold_value);
            state->draft.max_connections = parse(state->max_connections);
            state->draft.compression = state->compression;
            state->draft.threshold_unit = state->threshold_unit;
            state->draft.transfer_allocation = state->transfer_allocation;
            state->draft.conflict_policy = state->conflict_policy;
            state->draft.hash_on_transfer = state->hash_on_transfer;
            state->draft.hash_algo = state->hash_algo;
            state->draft.log_debug_level = state->log_debug_level;
            state->draft.log_raw_listing = state->log_raw_listing;
            state->draft.clamp();
            if (state->ctrl) state->ctrl->update_settings(state->draft);
            if (auto dropdown = active_dropdown_popup()) dismiss_dropdown(dropdown);
            clear_guard();
            if (auto popup = state->popup_wp.lock()) popup->base::detach();
        };

        auto scroll_page = [](component content)
        {
            auto page = grid::ctor({
                .columns = { { .weight = 1 } },
                .rows = { { .weight = 1 } },
                .handle_mode = grid_handle_mode::hidden,
                .column_padding_width = 0,
                .row_padding_height = 1,
                .border = faux,
            });
            page->attach(make_scrollview({ .content = std::move(content) }),
                         { .column = 0, .row = 0 });
            return component{ page };
        };
        auto pages = std::vector<tab_page_cfg>{};
        pages.push_back(make_tab_page(scroll_page(make_connection_page(state, accept, close)),
                                      []{ return text{ "Connection" }; }));
        pages.push_back(make_tab_page(scroll_page(make_sftp_page(state, accept, close)),
                                      []{ return text{ "SFTP" }; }));
        pages.push_back(make_tab_page(scroll_page(make_site_page(state, close)),
                                      []{ return text{ "Site" }; }));
        pages.push_back(make_tab_page(scroll_page(make_debug_page(state)),
                                      []{ return text{ "Debug" }; }));
        auto tabs = make_tabs({
            .pages = std::move(pages),
            .active = 0,
            .position = tab_position::top,
        });

        auto title = make_settings_label("Settings", label_role::text, label_overflow::clip, {
            .text = theme::title_fg,
            .background = theme::header,
        });
        auto buttons = flex::ctor({
            .direction = flex_direction::row,
            .justify_content = flex_justify::end,
            .align_items = flex_align::stretch,
            .column_gap = 1,
        });
        buttons->attach(make_button({
            .label = []{ return text{ " OK " }; },
            .on_activate = [accept](hids&, ui::base&){ accept(); },
        }), { .basis = 6, .minimum = 6, .maximum = 6 });
        buttons->attach(make_button({
            .label = []{ return text{ " Cancel " }; },
            .on_activate = [close](hids&, ui::base&){ close(); },
        }), { .basis = 10, .minimum = 10, .maximum = 10 });

        auto popup = make_dialog({
            .title = std::move(title),
            .content = std::move(tabs),
            .buttons = { buttons },
            .position = dialog_anchor::center,
            .size = {
                .width = dialog_length::ratio(0.80),
                .height = dialog_length::ratio(0.80),
            },
            .minimum = { 76, 18 },
            .maximum = { 104, 42 },
            .on_cancel = [state, clear_guard]
            {
                if (!std::exchange(state->done, true))
                {
                    if (auto dropdown = active_dropdown_popup()) dismiss_dropdown(dropdown);
                    clear_guard();
                }
            },
        });
        state->popup_wp = ptr::shadow(popup.widget);
        if (card_out) *card_out = popup.widget;
        return popup.widget;
    }
}
