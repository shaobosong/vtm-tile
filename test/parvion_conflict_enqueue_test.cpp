// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps/parvion/conflict_enqueue.hpp"

#include <cstdio>
#include <set>

using namespace netxs::app::parvion;
using netxs::si32;
using netxs::text;
using netxs::ui64;

namespace
{
    #define REQUIRE(expr) do { if (!(expr)) { std::fprintf(stderr, "failed at line %d: %s\n", __LINE__, #expr); return false; } } while (false)

    auto request(text path) -> transfer_request
    {
        return { true, std::move(path), "/source", "/dest", 100, 100 };
    }

    struct pump_fixture
    {
        std::vector<std::pair<text, conflict_action>> commits;
        std::function<void(conflict_choice)> reply;
        std::set<text> occupied;
        si32 asks = 0;
        si32 uniquifies = 0;

        auto probe(transfer_request const& req) const -> conflict_inputs
        {
            auto in = conflict_inputs{};
            in.download = req.download;
            in.source_path = req.remote_path;
            in.dest_path = req.local_path;
            in.dest_exists = req.local_path != "missing";
            return in;
        }
        auto uniquify(transfer_request req) -> std::optional<transfer_request>
        {
            ++uniquifies;
            for (auto number = ui64{ 1 }; number <= 10000; ++number)
            {
                auto candidate = numbered_name(req.local_path, number);
                if (!occupied.contains(candidate))
                {
                    req.local_path = std::move(candidate);
                    return req;
                }
            }
            return {};
        }
        void commit(transfer_request req, conflict_action action)
        {
            commits.emplace_back(std::move(req.local_path), action);
        }
        auto ask(text const&, std::function<void(conflict_choice)> answer)
        {
            ++asks;
            reply = std::move(answer);
        }
    };

    template<class Fixture>
    void pump(std::shared_ptr<enqueue_batch> const& batch, Fixture& fixture)
    {
        pump_enqueue_batch(batch,
            [&fixture](transfer_request const& req){ return fixture.probe(req); },
            [&fixture](transfer_request req){ return fixture.uniquify(std::move(req)); },
            [&fixture](transfer_request req, conflict_action action){ fixture.commit(std::move(req), action); },
            [&fixture](text const& dest, std::function<void(conflict_choice)> answer)
            {
                fixture.ask(dest, std::move(answer));
            });
    }

    auto test_skip_and_rename() -> bool
    {
        auto skip_batch = std::make_shared<enqueue_batch>();
        skip_batch->policy = conflict_skip;
        skip_batch->pending.push_back(request("exists"));
        auto skip_fixture = pump_fixture{};
        pump(skip_batch, skip_fixture);
        REQUIRE(skip_fixture.commits.size() == 1);
        REQUIRE(skip_fixture.commits[0].second == skip);

        auto rename_batch = std::make_shared<enqueue_batch>();
        rename_batch->policy = conflict_rename;
        rename_batch->pending.push_back(request("exists"));
        auto rename_fixture = pump_fixture{};
        pump(rename_batch, rename_fixture);
        REQUIRE(rename_fixture.uniquifies == 1);
        REQUIRE(rename_fixture.commits.size() == 1);
        REQUIRE(rename_fixture.commits[0].first == "exists (1)");
        REQUIRE(rename_fixture.commits[0].second == rename_dest);
        return true;
    }

    auto test_ask_defers_and_all_actions() -> bool
    {
        auto batch = std::make_shared<enqueue_batch>();
        batch->policy = conflict_ask;
        batch->pending.push_back(request("first"));
        batch->pending.push_back(request("second"));
        auto fixture = pump_fixture{};
        pump(batch, fixture);
        REQUIRE(batch->waiting && fixture.asks == 1 && fixture.commits.empty());
        pump(batch, fixture); // Nested/repeated pump must leave the unresolved request alone.
        REQUIRE(fixture.commits.empty() && fixture.asks == 1);
        REQUIRE((bool)fixture.reply);
        fixture.reply(conflict_choice::overwrite_all);
        REQUIRE(!batch->waiting && fixture.commits.size() == 2);
        REQUIRE(fixture.commits[0].second == write_dest);
        REQUIRE(fixture.commits[1].second == write_dest);
        REQUIRE(fixture.asks == 1);

        auto skip_batch = std::make_shared<enqueue_batch>();
        skip_batch->policy = conflict_ask;
        skip_batch->pending.push_back(request("first"));
        skip_batch->pending.push_back(request("second"));
        auto skip_fixture = pump_fixture{};
        pump(skip_batch, skip_fixture);
        skip_fixture.reply(conflict_choice::skip_all);
        REQUIRE(skip_fixture.commits.size() == 2);
        REQUIRE(skip_fixture.commits[0].second == skip);
        REQUIRE(skip_fixture.commits[1].second == skip);
        return true;
    }

    auto test_cancel_and_batch_isolation() -> bool
    {
        auto cancel_batch = std::make_shared<enqueue_batch>();
        cancel_batch->policy = conflict_ask;
        cancel_batch->pending.push_back(request("first"));
        cancel_batch->pending.push_back(request("second"));
        auto cancel_fixture = pump_fixture{};
        pump(cancel_batch, cancel_fixture);
        cancel_fixture.reply(conflict_choice::cancel_rest);
        REQUIRE(cancel_batch->cancelled && cancel_batch->pending.empty());
        REQUIRE(cancel_fixture.commits.empty());

        auto first = std::make_shared<enqueue_batch>();
        auto second = std::make_shared<enqueue_batch>();
        first->policy = second->policy = conflict_ask;
        first->pending.push_back(request("first"));
        second->pending.push_back(request("second"));
        auto first_fixture = pump_fixture{};
        auto second_fixture = pump_fixture{};
        pump(first, first_fixture);
        first_fixture.reply(conflict_choice::skip_all);
        pump(second, second_fixture);
        REQUIRE(second_fixture.asks == 1);
        second_fixture.reply(conflict_choice::overwrite);
        REQUIRE(first_fixture.commits[0].second == skip);
        REQUIRE(second_fixture.commits[0].second == write_dest);
        return true;
    }

    auto test_rename_exhaustion() -> bool
    {
        auto batch = std::make_shared<enqueue_batch>();
        batch->policy = conflict_rename;
        batch->pending.push_back(request("exists"));
        auto fixture = pump_fixture{};
        for (auto number = ui64{ 1 }; number <= 10000; ++number)
            fixture.occupied.insert(numbered_name("exists", number));
        pump(batch, fixture);
        REQUIRE(fixture.uniquifies == 1);
        REQUIRE(fixture.commits.empty());

        auto ask_batch = std::make_shared<enqueue_batch>();
        ask_batch->policy = conflict_ask;
        ask_batch->pending.push_back(request("first"));
        auto ask_fixture = pump_fixture{};
        for (auto number = ui64{ 1 }; number <= 10000; ++number)
            ask_fixture.occupied.insert(numbered_name("first", number));
        pump(ask_batch, ask_fixture);
        REQUIRE(ask_fixture.asks == 1 && (bool)ask_fixture.reply);
        ask_fixture.reply(conflict_choice::rename);
        REQUIRE(ask_fixture.uniquifies == 1);
        REQUIRE(ask_fixture.commits.empty());
        return true;
    }

    auto test_rename_all() -> bool
    {
        auto batch = std::make_shared<enqueue_batch>();
        batch->policy = conflict_ask;
        batch->pending.push_back(request("first"));
        batch->pending.push_back(request("second"));
        auto fixture = pump_fixture{};
        pump(batch, fixture);
        REQUIRE(batch->waiting && fixture.asks == 1);
        fixture.reply(conflict_choice::rename_all);
        REQUIRE(!batch->waiting && fixture.asks == 1);
        REQUIRE(fixture.uniquifies == 2);
        REQUIRE(fixture.commits.size() == 2);
        REQUIRE(fixture.commits[0].second == rename_dest);
        REQUIRE(fixture.commits[1].second == rename_dest);
        return true;
    }
}

int main()
{
    auto failed = 0;
    for (auto test : { test_skip_and_rename, test_ask_defers_and_all_actions,
                       test_cancel_and_batch_isolation, test_rename_all,
                       test_rename_exhaustion })
        if (!test()) ++failed;
    std::puts(failed ? "FAILED" : "OK");
    return failed ? 1 : 0;
}
