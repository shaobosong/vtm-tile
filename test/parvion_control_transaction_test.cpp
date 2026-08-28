// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps.hpp"

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    using remote = sftp_remote;
    using txn_t = remote::control_transaction;
    using rec_download = txn_t::rec_download;
    using rec_delete = txn_t::rec_delete;
    using rec_upload = txn_t::rec_upload;

    template<class Flow>
    concept has_walk_listing = requires { Flow::phase_t::listing; };
    template<class Flow>
    concept has_mkdir_probe = requires { Flow::phase_t::probing_mkdir; };

    static_assert(std::is_move_assignable_v<txn_t>);
    static_assert(has_walk_listing<rec_download>);
    static_assert(has_walk_listing<rec_delete>);
    static_assert(!has_walk_listing<rec_upload>);
    static_assert(!has_mkdir_probe<rec_download>);
    static_assert(has_mkdir_probe<rec_upload>);

    #define REQUIRE(expr) do { if (!(expr)) { std::fprintf(stderr, "  failed at line %d: %s\n", __LINE__, #expr); return false; } } while (false)

    auto done(view code) -> sftp_msg
    {
        auto m = sftp_msg{};
        m.type = sftp_evt::done;
        m.line.emplace_back(code);
        return m;
    }

    auto listing(text name, bool directory) -> sftp_msg
    {
        auto m = sftp_msg{};
        m.type = sftp_evt::listentry;
        m.list_name = std::move(name);
        m.list_text = directory
                    ? "drwxr-xr-x 1 owner group 0 Jan 1 directory"
                    : "-rw-r--r-- 1 owner group 1 Jan 1 file";
        return m;
    }

    void arm_connected(remote& r)
    {
        r.session.running.store(true);
        r.stage = remote::s_connected;
        r.path = "/";
        r.keepalive_sec = 0;
        r.response_timeout_sec = 0;
    }

    struct test_runner
    {
        int failures = 0;

        template<class Test>
        void run(view name, Test&& test)
        {
            if (!test())
            {
                std::fprintf(stderr, "FAIL: %.*s\n", (int)name.size(), name.data());
                ++failures;
            }
        }
    };
}

int main()
{
    auto tests = test_runner{};

    tests.run("transaction ownership and typed recursive collection", []
    {
        auto txn = txn_t{};
        REQUIRE(!txn.active());

        txn.start<txn_t::connection>();
        REQUIRE(txn.is<txn_t::connection>());
        REQUIRE(txn.is<txn_t::connection>(txn_t::connection::phase_t::waiting_greeting));
        REQUIRE(!txn.wire_busy());
        txn.command = remote::c_open;
        REQUIRE(txn.wire_busy());

        txn.start<txn_t::refresh>();
        REQUIRE(txn.is<txn_t::refresh>() && !txn.wire_busy());
        txn.start<txn_t::mutation>();
        REQUIRE(txn.is<txn_t::mutation>());
        txn.start<txn_t::keepalive>();
        REQUIRE(txn.is<txn_t::keepalive>());
        txn.reset();
        REQUIRE(!txn.active() && !txn.wire_busy());

        auto download = txn.ensure_collecting<rec_download>();
        REQUIRE(download && txn.recursive());
        REQUIRE(txn.ensure_collecting<rec_download>() == download);
        REQUIRE(!txn.ensure_collecting<rec_delete>());
        download->phase = rec_download::phase_t::listing;
        REQUIRE(!txn.ensure_collecting<rec_download>());

        txn.command = remote::c_ls;
        txn.staged.push_back({});
        auto& nav = txn.start<txn_t::navigation>("/", "/next");
        REQUIRE(txn.command == remote::c_none && txn.staged.empty());
        REQUIRE(txn.is<txn_t::navigation>(txn_t::navigation::phase_t::changing_directory));
        REQUIRE(nav.phase == txn_t::navigation::phase_t::changing_directory);

        txn.reset();
        REQUIRE(!txn.active());
        REQUIRE(txn.ensure_collecting<rec_upload>());
        return true;
    });

    tests.run("structured recursive command formatting", []
    {
        REQUIRE(rec_command_line({ rec_op::mkdir, "/a\"b" }) == "mkdir \"/a\"\"b\"");
        REQUIRE(rec_command_line({ rec_op::rm, "/a b" }) == "rm \"/a b\"");
        REQUIRE(rec_command_line({ rec_op::rmdir, "/dir" }) == "rmdir \"/dir\"");
        return true;
    });

    tests.run("list entries require a listing phase", []
    {
        auto r = remote{};
        arm_connected(r);
        auto rec = r.txn.ensure_collecting<rec_download>();
        REQUIRE(rec);
        r.txn.command = remote::c_rls;
        r.apply_event(listing("ignored", false));
        REQUIRE(r.txn.staged.empty());
        rec->phase = rec_download::phase_t::listing;
        r.apply_event(listing("accepted", false));
        REQUIRE(r.txn.staged.size() == 1 && r.txn.staged[0].name == "accepted");
        return true;
    });

    tests.run("navigation listing failure rolls back", []
    {
        auto r = remote{};
        arm_connected(r);
        REQUIRE(r.begin_navigation("/next"));
        REQUIRE(r.txn.is<txn_t::navigation>(txn_t::navigation::phase_t::changing_directory));
        REQUIRE(r.txn.command == remote::c_cd);
        r.apply_event(done("1"));
        REQUIRE(r.txn.is<txn_t::navigation>(txn_t::navigation::phase_t::listing_destination));
        REQUIRE(r.txn.command == remote::c_ls);
        r.apply_event(done("0"));
        REQUIRE(r.txn.is<txn_t::navigation>(txn_t::navigation::phase_t::rolling_back));
        REQUIRE(r.txn.command == remote::c_cd_rollback);
        return true;
    });

    tests.run("failed mutation reconciles and releases", []
    {
        auto r = remote{};
        arm_connected(r);
        REQUIRE(r.begin_mutation("mv \"/a\" \"/b\"", "Renaming..."));
        r.apply_event(done("0"));
        REQUIRE(r.txn.is<txn_t::mutation>(txn_t::mutation::phase_t::listing_after_failure));
        REQUIRE(r.txn.command == remote::c_ls);
        r.apply_event(done("1"));
        REQUIRE(!r.txn.active());
        REQUIRE(r.status.find("reconciled") != text::npos);
        return true;
    });

    tests.run("failed upload mkdir aborts and reconciles", []
    {
        auto r = remote{};
        arm_connected(r);
        auto rec = r.txn.ensure_collecting<rec_upload>();
        REQUIRE(rec);
        rec->commands.push_back({ rec_op::mkdir, "/partial" });
        rec->commands.push_back({ rec_op::mkdir, "/partial/child" });
        rec->command_i = 1;
        rec->uploads.push_back({});
        rec->phase = rec_upload::phase_t::executing;
        r.txn.command = remote::c_recop;
        r.apply_event(done("0"));
        REQUIRE(r.txn.is<rec_upload>(rec_upload::phase_t::probing_mkdir));
        REQUIRE(r.txn.command == remote::c_rls);
        r.apply_event(done("0"));
        REQUIRE(r.txn.is<rec_upload>(rec_upload::phase_t::listing_after_abort));
        REQUIRE(r.txn.command == remote::c_ls);
        auto partial = direntry{};
        partial.name = "partial";
        partial.is_dir = true;
        r.txn.staged.push_back(partial);
        r.apply_event(done("1"));
        REQUIRE(!r.txn.active() && r.queue.empty());
        REQUIRE(r.items.size() == 1 && r.items[0].name == "partial");
        REQUIRE(r.status.find("reconciled") != text::npos);
        return true;
    });

    tests.run("existing mkdir target directory advances without enqueue", []
    {
        auto r = remote{};
        arm_connected(r);
        auto rec = r.txn.ensure_collecting<rec_upload>();
        REQUIRE(rec);
        rec->commands.push_back({ rec_op::mkdir, "/upload_src" });
        rec->uploads.push_back({});
        rec->phase = rec_upload::phase_t::executing;
        r.txn.command = remote::c_recop;
        r.apply_event(done("0"));
        REQUIRE(r.txn.is<rec_upload>(rec_upload::phase_t::probing_mkdir));
        auto existing = direntry{};
        existing.name = "upload_src";
        existing.is_dir = true;
        r.txn.staged.push_back(existing);
        r.apply_event(done("1"));
        REQUIRE(r.txn.is<rec_upload>(rec_upload::phase_t::collecting));
        rec = r.txn.get_if<rec_upload>();
        REQUIRE(rec && rec->command_i == 1);
        REQUIRE(r.queue.empty());
        return true;
    });

    tests.run("file at mkdir target aborts upload", []
    {
        auto r = remote{};
        arm_connected(r);
        auto rec = r.txn.ensure_collecting<rec_upload>();
        REQUIRE(rec);
        rec->commands.push_back({ rec_op::mkdir, "/upload_src" });
        rec->uploads.push_back({});
        rec->phase = rec_upload::phase_t::executing;
        r.txn.command = remote::c_recop;
        r.apply_event(done("0"));
        auto existing = direntry{};
        existing.name = "upload_src";
        existing.is_dir = false;
        r.txn.staged.push_back(existing);
        r.apply_event(done("1"));
        REQUIRE(r.txn.is<rec_upload>(rec_upload::phase_t::listing_after_abort));
        REQUIRE(r.txn.command == remote::c_ls);
        r.apply_event(done("1"));
        REQUIRE(!r.txn.active() && r.queue.empty());
        return true;
    });

    tests.run("active transaction cannot be replaced", []
    {
        auto r = remote{};
        arm_connected(r);
        REQUIRE(r.begin_navigation("/next"));
        REQUIRE(!r.launch<txn_t::mutation>(remote::c_op, "mv \"/a\" \"/b\"", true));
        REQUIRE(r.txn.is<txn_t::navigation>(txn_t::navigation::phase_t::changing_directory));
        REQUIRE(r.txn.command == remote::c_cd);
        auto idle = remote{};
        REQUIRE(!idle.issue_command(remote::c_ls, "ls"));
        return true;
    });

    tests.run("delete walk collects files and directories", []
    {
        auto r = remote{};
        arm_connected(r);
        auto rec = r.txn.ensure_collecting<rec_delete>();
        REQUIRE(rec);
        rec->current = { "/real_dir", {} };
        rec->phase = rec_delete::phase_t::listing;
        r.txn.command = remote::c_rls;
        auto sub = direntry{};
        sub.name = "sub";
        sub.is_dir = true;
        auto file = direntry{};
        file.name = "a.txt";
        r.txn.staged.push_back(sub);
        r.txn.staged.push_back(file);
        r.apply_event(done("1"));
        rec = r.txn.get_if<rec_delete>();
        REQUIRE(rec && rec->phase == rec_delete::phase_t::collecting);
        REQUIRE(rec->dirs.size() == 1 && rec->dirs[0] == "/real_dir/sub");
        REQUIRE(rec->files.size() == 1 && rec->files[0] == "/real_dir/a.txt");
        REQUIRE(rec->stack.size() == 1 && rec->stack.back().remote == "/real_dir/sub");
        return true;
    });

    tests.run("delete failures are reported after result listing", []
    {
        auto r = remote{};
        arm_connected(r);
        auto rec = r.txn.ensure_collecting<rec_delete>();
        REQUIRE(rec);
        rec->phase = rec_delete::phase_t::listing_result;
        rec->failures = 2;
        r.txn.command = remote::c_ls;
        r.apply_event(done("1"));
        REQUIRE(!r.txn.active());
        REQUIRE(r.status.find("Recursive delete") != text::npos);
        REQUIRE(r.status.find("2 failed command(s)") != text::npos);
        return true;
    });

    tests.run("post-auth path discovery failure keeps connection open", []
    {
        auto r = remote{};
        r.session.running.store(true);
        r.stage = remote::s_opening;
        auto& conn = r.txn.start<txn_t::connection>();
        conn.phase = txn_t::connection::phase_t::opening;
        r.txn.command = remote::c_open;
        r.apply_event(done("1"));
        REQUIRE(r.stage == remote::s_connected);
        REQUIRE(r.txn.is<txn_t::connection>(txn_t::connection::phase_t::discovering_path));
        REQUIRE(r.txn.command == remote::c_pwd);
        r.apply_event(done("0"));
        REQUIRE(r.stage == remote::s_connected && !r.txn.active());
        REQUIRE(r.session.alive());
        REQUIRE(r.status.find("connection remains open") != text::npos);
        return true;
    });

    tests.run("post-auth initial listing failure clears provisional view", []
    {
        auto r = remote{};
        arm_connected(r);
        auto& conn = r.txn.start<txn_t::connection>();
        conn.phase = txn_t::connection::phase_t::listing;
        conn.target = "/home/user";
        r.txn.command = remote::c_ls;
        r.txn.staged.push_back({});
        r.apply_event(done("0"));
        REQUIRE(r.stage == remote::s_connected && !r.txn.active());
        REQUIRE(r.session.alive() && r.path == "/home/user" && r.items.empty());
        return true;
    });

    tests.run("pre-auth open failure closes connection", []
    {
        auto r = remote{};
        r.session.running.store(true);
        r.stage = remote::s_opening;
        auto& conn = r.txn.start<txn_t::connection>();
        conn.phase = txn_t::connection::phase_t::opening;
        r.txn.command = remote::c_open;
        r.apply_event(done("0"));
        REQUIRE(r.stage == remote::s_failed && !r.session.alive() && !r.txn.active());
        return true;
    });

    tests.run("keyfile terminal failure advances through connection flow", []
    {
        auto r = remote{};
        r.session.running.store(true);
        r.stage = remote::s_opening;
        r.cfg.keyfiles.clear();
        auto& conn = r.txn.start<txn_t::connection>();
        conn.phase = txn_t::connection::phase_t::registering_key;
        r.txn.command = remote::c_keyfile;
        r.apply_event(done("0"));
        REQUIRE(r.txn.is<txn_t::connection>(txn_t::connection::phase_t::opening));
        REQUIRE(r.txn.command == remote::c_open);
        return true;
    });

    tests.run("dropped connection preserves reconnect transaction", []
    {
        auto r = remote{};
        r.stage = remote::s_connected;
        auto& conn = r.txn.start<txn_t::connection>();
        conn.phase = txn_t::connection::phase_t::listing;
        r.txn.command = remote::c_ls;
        r.poll();
        auto retry = r.txn.get_if<txn_t::connection>();
        REQUIRE(retry && retry->reconnect);
        REQUIRE(retry->phase == txn_t::connection::phase_t::retry_wait);
        REQUIRE(r.stage == remote::s_idle);
        return true;
    });

    tests.run("watchdog recovery preserves reconnect attempts and path", []
    {
        auto r = remote{};
        r.session.running.store(true);
        r.stage = remote::s_connected;
        r.path = "/kept";
        r.response_timeout_sec = 1;
        r.last_activity = remote::steady_clock::now() - std::chrono::seconds{ 2 };
        auto& conn = r.txn.start<txn_t::connection>(true, "/kept", 4);
        conn.phase = txn_t::connection::phase_t::listing;
        r.txn.command = remote::c_ls;
        r.poll();
        auto retry = r.txn.get_if<txn_t::connection>();
        REQUIRE(retry && retry->attempts == 4 && retry->restore_path == "/kept");
        REQUIRE(retry->phase == txn_t::connection::phase_t::retry_wait);
        REQUIRE(r.stage == remote::s_idle);
        return true;
    });

    tests.run("upload result listing preserves completion status", []
    {
        auto r = remote{};
        arm_connected(r);
        r.no_autostart = true;
        auto rec = r.txn.ensure_collecting<rec_upload>();
        REQUIRE(rec);
        rec->uploads.push_back({ "/local/source.bin", "/remote/target.bin", 123 });
        r.poll();
        REQUIRE(r.txn.is<rec_upload>(rec_upload::phase_t::listing_result));
        REQUIRE(r.txn.command == remote::c_ls);
        REQUIRE(r.status == "Remote directory tree prepared; queued 1 upload(s).");
        REQUIRE(r.queue.size() == 1);
        REQUIRE(r.queue[0].id != 0 && !r.queue[0].download);
        REQUIRE(r.queue[0].local_path == "/local/source.bin");
        REQUIRE(r.queue[0].remote_path == "/remote/target.bin");
        REQUIRE(r.queue[0].dest_dir == "/" && r.queue[0].size == 123);
        REQUIRE(r.queue[0].status == queue_item::queued);
        r.apply_event(done("1"));
        REQUIRE(!r.txn.active());
        REQUIRE(r.status == "Remote directory tree prepared; queued 1 upload(s).");
        return true;
    });

    tests.run("upload result refresh failure is reported", []
    {
        auto r = remote{};
        arm_connected(r);
        auto rec = r.txn.ensure_collecting<rec_upload>();
        REQUIRE(rec);
        rec->phase = rec_upload::phase_t::listing_result;
        rec->completion_status = "Remote directory tree prepared; queued 1 upload(s).";
        r.txn.command = remote::c_ls;
        r.apply_event(done("0"));
        REQUIRE(r.status.starts_with("Error:"));
        return true;
    });

    tests.run("secret prompt pauses watchdog and answer rearms it", []
    {
        auto r = remote{};
        r.session.running.store(true);
        r.stage = remote::s_opening;
        r.response_timeout_sec = 1;
        r.last_activity = remote::steady_clock::now() - std::chrono::seconds{ 2 };
        auto& conn = r.txn.start<txn_t::connection>();
        conn.phase = txn_t::connection::phase_t::opening;
        r.txn.command = remote::c_open;
        r.sec = remote::sec_pending;
        r.maybe_watchdog();
        REQUIRE(r.session.alive() && r.txn.command == remote::c_open);
        r.sec = remote::sec_shown;
        r.maybe_watchdog();
        REQUIRE(r.session.alive() && r.txn.command == remote::c_open);
        auto stale = r.last_activity;
        r.sec_req.is_passphrase = false;
        r.provide_secret("secret");
        REQUIRE(r.sec == remote::sec_idle && r.last_activity > stale);
        r.maybe_watchdog();
        REQUIRE(r.session.alive() && r.stage == remote::s_opening && r.txn.command == remote::c_open);
        return true;
    });

    if (tests.failures)
    {
        std::fprintf(stderr, "parvion control transaction tests failed: %d case(s)\n", tests.failures);
        return 1;
    }
    return 0;
}
