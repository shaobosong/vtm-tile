// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps.hpp"

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    auto done(view code) -> sftp_msg
    {
        auto m = sftp_msg{};
        m.type = sftp_evt::done;
        m.line.emplace_back(code);
        return m;
    }
    void arm_connected(sftp_remote& r)
    {
        r.session.running.store(true);
        r.stage = sftp_remote::s_connected;
        r.path = "/";
        r.keepalive_sec = 0;
        r.response_timeout_sec = 0;
    }
}

int main()
{
    using remote = sftp_remote;
    using txn_t = remote::control_transaction;
    using recursive = txn_t::recursive;

    static_assert(std::is_move_assignable_v<txn_t>);

    auto txn = txn_t{};
    if (txn.active() || txn.kind() != txn_t::kind_t::none) return 1;

    txn.start<txn_t::connection>();
    if (txn.kind() != txn_t::kind_t::connection || !txn.is<txn_t::connection>()
     || !txn.is<txn_t::connection>(txn_t::connection::phase_t::waiting_greeting)) return 2;
    if (txn.wire_busy()) return 3;
    txn.command = remote::c_open;
    if (!txn.wire_busy()) return 4;

    txn.start<txn_t::refresh>("/");
    if (txn.kind() != txn_t::kind_t::refresh || txn.wire_busy()) return 5;
    txn.start<txn_t::mutation>("/");
    if (txn.kind() != txn_t::kind_t::mutation) return 6;
    txn.start<txn_t::keepalive>();
    if (txn.kind() != txn_t::kind_t::keepalive) return 7;
    txn.reset();
    if (txn.wire_busy()) return 8;

    auto download = txn.ensure_collecting(recursive::mode_t::download);
    if (!download || txn.kind() != txn_t::kind_t::recursive) return 9;
    if (txn.ensure_collecting(recursive::mode_t::download) != download) return 10;
    if (txn.ensure_collecting(recursive::mode_t::delete_)) return 11;

    download->phase = recursive::phase_t::listing;
    if (txn.ensure_collecting(recursive::mode_t::download)) return 12;

    txn.command = remote::c_ls;
    txn.staged.push_back({});
    auto& nav = txn.start<txn_t::navigation>("/", "/next");
    if (txn.command != remote::c_none || !txn.staged.empty()) return 13;
    if (txn.kind() != txn_t::kind_t::navigation
     || !txn.is<txn_t::navigation>()
     || !txn.is<txn_t::navigation>(txn_t::navigation::phase_t::changing_directory)
     || nav.phase != txn_t::navigation::phase_t::changing_directory) return 14;

    txn.reset();
    if (txn.active() || txn.kind() != txn_t::kind_t::none) return 15;
    if (!txn.ensure_collecting(recursive::mode_t::upload)) return 16;

    auto idle = remote{};
    if (idle.issue_command(remote::c_ls, "ls")) return 17;

    auto r = remote{};
    arm_connected(r);
    if (!r.begin_navigation("/next")) return 18;
    if (!r.txn.is<txn_t::navigation>(txn_t::navigation::phase_t::changing_directory)) return 19;
    if (r.txn.command != remote::c_cd) return 20;
    r.apply_event(done("1"));
    if (!r.txn.is<txn_t::navigation>(txn_t::navigation::phase_t::listing_destination)) return 21;
    if (r.txn.command != remote::c_ls) return 22;
    r.apply_event(done("0"));
    if (!r.txn.is<txn_t::navigation>(txn_t::navigation::phase_t::rolling_back)) return 23;
    if (r.txn.command != remote::c_cd_rollback) return 24;

    auto mut = remote{};
    arm_connected(mut);
    if (!mut.begin_mutation("mv \"/a\" \"/b\"", "Renaming...")) return 25;
    mut.apply_event(done("0"));
    if (!mut.txn.is<txn_t::mutation>(txn_t::mutation::phase_t::listing_after_failure)) return 26;
    if (mut.txn.command != remote::c_ls) return 27;
    mut.apply_event(done("1"));
    if (mut.txn.active()) return 28;

    auto mkdir_fail = remote{};
    arm_connected(mkdir_fail);
    auto rec_fail = mkdir_fail.txn.ensure_collecting(recursive::mode_t::upload);
    if (!rec_fail) return 29;
    // The parent mkdir already succeeded; failure preparing its child must refresh the
    // committed pane so the partially created parent becomes visible.
    rec_fail->commands.push_back("mkdir \"/partial\"");
    rec_fail->commands.push_back("mkdir \"/partial/child\"");
    rec_fail->command_i = 1;
    rec_fail->uploads.push_back({});
    rec_fail->phase = recursive::phase_t::executing;
    mkdir_fail.txn.command = remote::c_recop;
    mkdir_fail.apply_event(done("0"));
    if (!mkdir_fail.txn.is<recursive>(recursive::phase_t::probing_mkdir)) return 30;
    if (mkdir_fail.txn.command != remote::c_rls) return 31;
    mkdir_fail.apply_event(done("0"));
    if (!mkdir_fail.txn.is<recursive>(recursive::phase_t::listing_after_abort)) return 32;
    if (mkdir_fail.txn.command != remote::c_ls) return 33;
    auto partial_root = direntry{}; partial_root.name = "partial"; partial_root.is_dir = true;
    mkdir_fail.txn.staged.push_back(partial_root);
    mkdir_fail.apply_event(done("1"));
    if (mkdir_fail.txn.active()) return 34;
    if (!mkdir_fail.queue.empty()) return 35;
    if (mkdir_fail.items.size() != 1 || mkdir_fail.items[0].name != "partial") return 36;
    if (mkdir_fail.status.find("reconciled") == text::npos) return 37;

    auto mkdir_exists = remote{};
    arm_connected(mkdir_exists);
    auto rec_ok = mkdir_exists.txn.ensure_collecting(recursive::mode_t::upload);
    if (!rec_ok) return 38;
    rec_ok->commands.push_back("mkdir \"/upload_src\"");
    rec_ok->uploads.push_back({});
    rec_ok->phase = recursive::phase_t::executing;
    mkdir_exists.txn.command = remote::c_recop;
    mkdir_exists.apply_event(done("0"));
    if (!mkdir_exists.txn.is<recursive>(recursive::phase_t::probing_mkdir)) return 39;
    auto existing_dir = direntry{}; existing_dir.name = "upload_src"; existing_dir.is_dir = true;
    mkdir_exists.txn.staged.push_back(existing_dir);
    mkdir_exists.apply_event(done("1"));
    if (!mkdir_exists.txn.is<recursive>(recursive::phase_t::collecting)) return 40;
    auto rec_after = mkdir_exists.txn.get_if<recursive>();
    if (!rec_after || rec_after->command_i != 1) return 41;
    if (mkdir_exists.queue.size() != 0) return 42; // enqueue happens only when the command list is drained

    auto mkdir_is_file = remote{};
    arm_connected(mkdir_is_file);
    auto rec_file = mkdir_is_file.txn.ensure_collecting(recursive::mode_t::upload);
    if (!rec_file) return 43;
    rec_file->commands.push_back("mkdir \"/upload_src\"");
    rec_file->uploads.push_back({});
    rec_file->phase = recursive::phase_t::executing;
    mkdir_is_file.txn.command = remote::c_recop;
    mkdir_is_file.apply_event(done("0"));
    auto existing_file = direntry{}; existing_file.name = "upload_src"; existing_file.is_dir = false;
    mkdir_is_file.txn.staged.push_back(existing_file);
    mkdir_is_file.apply_event(done("1"));
    if (!mkdir_is_file.txn.is<recursive>(recursive::phase_t::listing_after_abort)) return 44;
    if (mkdir_is_file.txn.command != remote::c_ls) return 45;
    mkdir_is_file.apply_event(done("1"));
    if (mkdir_is_file.txn.active() || !mkdir_is_file.queue.empty()) return 46;

    auto busy = remote{};
    arm_connected(busy);
    if (!busy.begin_navigation("/next")) return 47;
    if (busy.launch<txn_t::mutation>(remote::c_op, "mv \"/a\" \"/b\"", true, "/")) return 48;
    if (!busy.txn.is<txn_t::navigation>(txn_t::navigation::phase_t::changing_directory)) return 49;
    if (busy.txn.command != remote::c_cd) return 50;

    auto del_list = remote{};
    arm_connected(del_list);
    auto rec_del = del_list.txn.ensure_collecting(recursive::mode_t::delete_);
    if (!rec_del) return 43;
    rec_del->current = { "/real_dir", {} };
    rec_del->phase = recursive::phase_t::listing;
    del_list.txn.command = remote::c_rls;
    auto sub = direntry{}; sub.name = "sub"; sub.is_dir = true;
    auto file = direntry{}; file.name = "a.txt";
    del_list.txn.staged.push_back(sub);
    del_list.txn.staged.push_back(file);
    del_list.apply_event(done("1"));
    auto rec_listed = del_list.txn.get_if<recursive>();
    if (!rec_listed || rec_listed->phase != recursive::phase_t::collecting) return 44;
    if (rec_listed->delete_dirs.size() != 1 || rec_listed->delete_dirs[0] != "/real_dir/sub") return 45;
    if (rec_listed->delete_files.size() != 1 || rec_listed->delete_files[0] != "/real_dir/a.txt") return 46;
    if (rec_listed->stack.size() != 1 || rec_listed->stack.back().remote != "/real_dir/sub") return 47;

    auto up_list = remote{};
    arm_connected(up_list);
    auto rec_up = up_list.txn.ensure_collecting(recursive::mode_t::upload);
    if (!rec_up) return 48;
    rec_up->current = { "/upload_src", {} };
    rec_up->phase = recursive::phase_t::listing;
    up_list.txn.command = remote::c_rls;
    auto up_sub = direntry{}; up_sub.name = "sub"; up_sub.is_dir = true;
    up_list.txn.staged.push_back(up_sub);
    up_list.apply_event(done("1"));
    if (up_list.txn.active()) return 49;
    if (!up_list.queue.empty()) return 50;

    auto del_result = remote{};
    arm_connected(del_result);
    auto rec_result = del_result.txn.ensure_collecting(recursive::mode_t::delete_);
    if (!rec_result) return 51;
    rec_result->phase = recursive::phase_t::listing_result;
    rec_result->failures = 2;
    del_result.txn.command = remote::c_ls;
    del_result.apply_event(done("1"));
    if (del_result.txn.active()) return 52;
    if (del_result.status.find("Recursive delete") == text::npos
     || del_result.status.find("2 failed command(s)") == text::npos) return 53;

    auto dl_exec = remote{};
    arm_connected(dl_exec);
    auto rec_dl = dl_exec.txn.ensure_collecting(recursive::mode_t::download);
    if (!rec_dl) return 54;
    rec_dl->phase = recursive::phase_t::executing;
    rec_dl->commands.push_back("mkdir \"/x\"");
    dl_exec.txn.command = remote::c_recop;
    dl_exec.apply_event(done("0"));
    if (dl_exec.txn.active()) return 55;

    auto initial_pwd_fail = remote{};
    initial_pwd_fail.session.running.store(true);
    initial_pwd_fail.stage = remote::s_opening;
    auto& initial_conn = initial_pwd_fail.txn.start<txn_t::connection>();
    initial_conn.phase = txn_t::connection::phase_t::opening;
    initial_pwd_fail.txn.command = remote::c_open;
    initial_pwd_fail.apply_event(done("1"));
    if (initial_pwd_fail.stage != remote::s_connected) return 56;
    if (!initial_pwd_fail.txn.is<txn_t::connection>(txn_t::connection::phase_t::discovering_path)
     || initial_pwd_fail.txn.command != remote::c_pwd) return 57;
    initial_pwd_fail.apply_event(done("0"));
    if (initial_pwd_fail.stage != remote::s_connected || initial_pwd_fail.txn.active()) return 58;
    if (!initial_pwd_fail.session.alive()
     || initial_pwd_fail.status.find("connection remains open") == text::npos) return 59;

    auto initial_ls_fail = remote{};
    arm_connected(initial_ls_fail);
    auto& listing_conn = initial_ls_fail.txn.start<txn_t::connection>();
    listing_conn.phase = txn_t::connection::phase_t::listing;
    listing_conn.target = "/home/user";
    initial_ls_fail.txn.command = remote::c_ls;
    initial_ls_fail.txn.staged.push_back({});
    initial_ls_fail.apply_event(done("0"));
    if (initial_ls_fail.stage != remote::s_connected || initial_ls_fail.txn.active()) return 60;
    if (!initial_ls_fail.session.alive() || initial_ls_fail.path != "/home/user"
     || !initial_ls_fail.items.empty()) return 61;

    auto preauth_fail = remote{};
    preauth_fail.session.running.store(true);
    preauth_fail.stage = remote::s_opening;
    auto& preauth_conn = preauth_fail.txn.start<txn_t::connection>();
    preauth_conn.phase = txn_t::connection::phase_t::opening;
    preauth_fail.txn.command = remote::c_open;
    preauth_fail.apply_event(done("0"));
    if (preauth_fail.stage != remote::s_failed || preauth_fail.session.alive()
     || preauth_fail.txn.active()) return 62;

    auto postauth_drop = remote{};
    postauth_drop.stage = remote::s_connected;
    auto& dropped_conn = postauth_drop.txn.start<txn_t::connection>();
    dropped_conn.phase = txn_t::connection::phase_t::listing;
    postauth_drop.txn.command = remote::c_ls;
    postauth_drop.poll();
    auto retry = postauth_drop.txn.get_if<txn_t::connection>();
    if (!retry || !retry->reconnect || retry->phase != txn_t::connection::phase_t::retry_wait
     || postauth_drop.stage != remote::s_idle) return 63;

    auto interrupted_restore = remote{};
    interrupted_restore.session.running.store(true);
    interrupted_restore.stage = remote::s_connected;
    interrupted_restore.path = "/kept";
    interrupted_restore.response_timeout_sec = 1;
    interrupted_restore.last_activity = remote::steady_clock::now() - std::chrono::seconds{ 2 };
    auto& restore_conn = interrupted_restore.txn.start<txn_t::connection>(true, "/kept", 4);
    restore_conn.phase = txn_t::connection::phase_t::listing;
    interrupted_restore.txn.command = remote::c_ls;
    interrupted_restore.poll();
    retry = interrupted_restore.txn.get_if<txn_t::connection>();
    if (!retry || retry->attempts != 4 || retry->restore_path != "/kept"
     || retry->phase != txn_t::connection::phase_t::retry_wait
     || interrupted_restore.stage != remote::s_idle) return 64;

    auto upload_result = remote{};
    arm_connected(upload_result);
    upload_result.no_autostart = true;
    auto upload = upload_result.txn.ensure_collecting(recursive::mode_t::upload);
    if (!upload) return 65;
    upload->uploads.push_back({});
    upload_result.poll();
    if (!upload_result.txn.is<recursive>(recursive::phase_t::listing_result)
     || upload_result.txn.command != remote::c_ls
     || upload_result.status != "Remote directory tree prepared; queued 1 upload(s).") return 66;
    upload_result.apply_event(done("1"));
    if (upload_result.txn.active()
     || upload_result.status != "Remote directory tree prepared; queued 1 upload(s).") return 67;

    auto upload_refresh_fail = remote{};
    arm_connected(upload_refresh_fail);
    upload = upload_refresh_fail.txn.ensure_collecting(recursive::mode_t::upload);
    if (!upload) return 68;
    upload->phase = recursive::phase_t::listing_result;
    upload->completion_status = "Remote directory tree prepared; queued 1 upload(s).";
    upload_refresh_fail.txn.command = remote::c_ls;
    upload_refresh_fail.apply_event(done("0"));
    if (upload_refresh_fail.status.find("Error:") != 0) return 69;

    auto prompted_auth = remote{};
    prompted_auth.session.running.store(true);
    prompted_auth.stage = remote::s_opening;
    prompted_auth.response_timeout_sec = 1;
    prompted_auth.last_activity = remote::steady_clock::now() - std::chrono::seconds{ 2 };
    auto& prompted_conn = prompted_auth.txn.start<txn_t::connection>();
    prompted_conn.phase = txn_t::connection::phase_t::opening;
    prompted_auth.txn.command = remote::c_open;
    prompted_auth.sec = remote::sec_pending;
    prompted_auth.maybe_watchdog();
    if (!prompted_auth.session.alive() || prompted_auth.stage != remote::s_opening
     || prompted_auth.txn.command != remote::c_open) return 70;
    prompted_auth.sec = remote::sec_shown;
    prompted_auth.maybe_watchdog();
    if (!prompted_auth.session.alive() || prompted_auth.stage != remote::s_opening
     || prompted_auth.txn.command != remote::c_open) return 71;

    auto stale_activity = prompted_auth.last_activity;
    prompted_auth.sec_req.is_passphrase = false;
    prompted_auth.provide_secret("secret");
    if (prompted_auth.sec != remote::sec_idle || prompted_auth.last_activity <= stale_activity) return 72;
    prompted_auth.maybe_watchdog();
    if (!prompted_auth.session.alive() || prompted_auth.stage != remote::s_opening
     || prompted_auth.txn.command != remote::c_open) return 73;
    return 0;
}
