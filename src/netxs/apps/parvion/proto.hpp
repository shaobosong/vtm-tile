// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/proto.hpp: A self-contained port of the FileZilla/PARVION "fzprintf" machine
// protocol (the line-based contract between the engine and the parvionsftp helper).
//   - sftp_evt      : engine-side mirror of the child's sftpEventTypes. The wire
//                     encodes the type as ('0' + ordinal), so the order is fixed.
//   - sftp_parser   : incremental decoder, mirroring SftpInputParser::OnData().
//   - quote_name    : filename quoting for the parent->child command channel.
// Ported from fz-project: src/FileZilla3/src/putty/fzprintf.{c,h},
// src/engine/sftp/event.h, src/engine/sftp/input_parser.cpp.  Protocol v12.

#include <functional>
#include <vector>

namespace netxs::app::parvion
{
    static constexpr auto fzsftp_protocol_version = 12;

    // Child -> parent message types. Ordinals MUST match event.h / fzprintf.h.
    enum class sftp_evt : si32
    {
        unknown               = -1,
        reply                 = 0,  // Command succeeded; payload is the reply text.
        done                  = 1,  // Command finished; payload is the result code.
        error                 = 2,  // Command failed; payload is the error text.
        verbose               = 3,  // Verbose log line.
        info                  = 4,  // Info log line.
        status                = 5,  // Status log line.
        recv                  = 6,  // Socket recv notification (unused in this fork).
        send                  = 7,  // Socket send notification (unused in this fork).
        listentry             = 8,  // One directory entry: {text, mtime, name}.
        ask_hostkey           = 9,  // Host key prompt (2 lines).
        ask_hostkey_changed   = 10, // Host key changed prompt (2 lines).
        ask_hostkey_betteralg = 11, // Better host-key algorithm prompt (2 lines).
        ask_password          = 12, // Password prompt.
        transfer              = 13, // Transfer progress: bytes written/acked.
        request_preamble      = 14,
        request_instruction   = 15,
        used_quota_recv       = 16, // Quota notifications (0-line; bare type byte).
        used_quota_send       = 17,
        kex_algorithm         = 18,
        kex_hash              = 19,
        kex_curve             = 20,
        cipher_cts            = 21,
        cipher_stc            = 22,
        mac_cts               = 23,
        mac_stc               = 24,
        hostkey               = 25,
        io_size               = 26, // Shared-memory transfer path (deferred).
        io_open               = 27,
        io_nextbuf            = 28,
        io_finalize           = 29,
        count                 = 30,
    };

    // Number of payload lines following the type byte (per input_parser.cpp).
    inline auto sftp_lines(sftp_evt e) -> si32
    {
        switch (e)
        {
            case sftp_evt::used_quota_recv:
            case sftp_evt::used_quota_send:
            case sftp_evt::io_size:
                return 0;
            case sftp_evt::ask_hostkey:
            case sftp_evt::ask_hostkey_changed:
            case sftp_evt::ask_hostkey_betteralg:
                return 2;
            case sftp_evt::listentry:
                return 3;
            case sftp_evt::unknown:
            case sftp_evt::count:
                return 0;
            default:
                return 1;
        }
    }

    // Short event name for debug-level wire tracing in the message log.
    inline auto sftp_evt_name(sftp_evt e) -> view
    {
        switch (e)
        {
            case sftp_evt::reply:                 return "reply";
            case sftp_evt::done:                  return "done";
            case sftp_evt::error:                 return "error";
            case sftp_evt::verbose:               return "verbose";
            case sftp_evt::info:                  return "info";
            case sftp_evt::status:                return "status";
            case sftp_evt::recv:                  return "recv";
            case sftp_evt::send:                  return "send";
            case sftp_evt::listentry:             return "listentry";
            case sftp_evt::ask_hostkey:           return "ask_hostkey";
            case sftp_evt::ask_hostkey_changed:   return "ask_hostkey_changed";
            case sftp_evt::ask_hostkey_betteralg: return "ask_hostkey_betteralg";
            case sftp_evt::ask_password:          return "ask_password";
            case sftp_evt::transfer:              return "transfer";
            case sftp_evt::request_preamble:      return "request_preamble";
            case sftp_evt::request_instruction:   return "request_instruction";
            case sftp_evt::used_quota_recv:       return "used_quota_recv";
            case sftp_evt::used_quota_send:       return "used_quota_send";
            case sftp_evt::kex_algorithm:         return "kex_algorithm";
            case sftp_evt::kex_hash:              return "kex_hash";
            case sftp_evt::kex_curve:             return "kex_curve";
            case sftp_evt::cipher_cts:            return "cipher_cts";
            case sftp_evt::cipher_stc:            return "cipher_stc";
            case sftp_evt::mac_cts:               return "mac_cts";
            case sftp_evt::mac_stc:               return "mac_stc";
            case sftp_evt::hostkey:               return "hostkey";
            case sftp_evt::io_size:               return "io_size";
            case sftp_evt::io_open:               return "io_open";
            case sftp_evt::io_nextbuf:            return "io_nextbuf";
            case sftp_evt::io_finalize:           return "io_finalize";
            default:                              return "unknown";
        }
    }

    // A fully parsed protocol message.
    struct sftp_msg
    {
        sftp_evt          type = sftp_evt::unknown;
        std::vector<text> line;        // Normal events: the event's text line(s).
        text              list_text;   // Listentry: raw listing line (perms/size/...).
        text              list_name;   // Listentry: file name.
        ui64              list_mtime = 0; // Listentry: modification time (epoch s).

        auto first() const -> view { return line.empty() ? view{} : view{ line.front() }; }
    };

    // Incremental decoder. Feed raw child stdout bytes; on_msg fires once per
    // complete message. Safe to feed arbitrary chunk boundaries.
    struct sftp_parser
    {
        std::function<void(sftp_msg&&)> on_msg;

        void feed(view bytes)
        {
            if (head_ && head_ == buf_.size()) { buf_.clear(); head_ = 0; }
            else if (head_ > 65536)            { buf_.erase(0, head_); head_ = 0; }
            buf_.append(bytes.data(), bytes.size());
            parse();
        }

    private:
        text     buf_;
        size_t   head_ = 0;
        bool     in_msg_ = faux;
        sftp_msg cur_;
        si32     need_ = 0;
        si32     pending_ = 0;

        auto avail() const { return buf_.size() - head_; }

        static auto to_u64(view s) -> ui64
        {
            auto v = ui64{};
            for (auto c : s) { if (c < '0' || c > '9') break; v = v * 10 + (ui64)(c - '0'); }
            return v;
        }

        void emit()
        {
            in_msg_ = faux;
            auto m = std::move(cur_);
            cur_ = sftp_msg{};
            if (on_msg) on_msg(std::move(m));
        }

        void parse()
        {
            for (;;)
            {
                if (!in_msg_)
                {
                    if (avail() == 0) return;
                    auto code = (si32)(unsigned char)buf_[head_] - '0';
                    ++head_;
                    if (code <= (si32)sftp_evt::unknown || code >= (si32)sftp_evt::count) continue; // Drop stray byte.
                    cur_ = sftp_msg{};
                    cur_.type = (sftp_evt)code;
                    need_ = sftp_lines(cur_.type);
                    pending_ = need_;
                    in_msg_ = true;
                    if (pending_ == 0) { emit(); continue; }
                }
                else
                {
                    auto rest = view{ buf_ }.substr(head_);
                    auto nl = rest.find('\n');
                    if (nl == view::npos) return; // Need more bytes.
                    auto ln = rest.substr(0, nl);
                    if (!ln.empty() && ln.back() == '\r') ln.remove_suffix(1);
                    auto i = need_ - pending_;
                    --pending_;
                    if (cur_.type == sftp_evt::listentry)
                    {
                             if (i == 1) cur_.list_mtime = to_u64(ln);
                        else if (i == 2) cur_.list_name = text{ ln };
                        else             cur_.list_text = text{ ln };
                    }
                    else cur_.line.emplace_back(ln);
                    head_ += nl + 1;
                    if (pending_ == 0) emit();
                }
            }
        }
    };

    // Quote a path/filename for the command channel (FileZilla QuoteFilename:
    // wrap in double-quotes, doubling any embedded quotes).
    inline auto quote_name(view s) -> text
    {
        auto out = text{ "\"" };
        for (auto c : s) { if (c == '"') out += "\"\""; else out.push_back(c); }
        out.push_back('"');
        return out;
    }
}
