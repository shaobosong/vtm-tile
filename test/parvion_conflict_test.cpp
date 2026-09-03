// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps/parvion/conflict.hpp"

#include <cstdio>

using namespace netxs::app::parvion;
using netxs::si64;

namespace
{
    #define REQUIRE(expr) do { \
        if (!(expr)) { std::fprintf(stderr, "failed at line %d: %s\n", __LINE__, #expr); return false; } \
    } while (false)

    auto existing(si64 size = 10, time_t mtime = 100) -> conflict_inputs
    {
        auto in = conflict_inputs{};
        in.dest_exists = true;
        in.source_size = 100;
        in.source_mtime = 100;
        in.dest_size = size;
        in.dest_mtime = mtime;
        return in;
    }

    auto test_overwrite_and_skip() -> bool
    {
        REQUIRE(classify(conflict_overwrite, existing()) == write_dest);
        REQUIRE(classify(conflict_skip, existing()) == skip);
        auto missing = conflict_inputs{};
        REQUIRE(classify(conflict_skip, missing) == write_dest);
        return true;
    }

    auto test_newer() -> bool
    {
        auto in = existing(50, 50);
        REQUIRE(classify(conflict_newer, in) == write_dest);
        in.source_mtime = 50;
        in.dest_mtime = 100;
        REQUIRE(classify(conflict_newer, in) == skip);
        in.dest_mtime = 0;
        REQUIRE(classify(conflict_newer, in) == write_dest);
        in.dest_mtime = 100;
        in.source_mtime = 0;
        REQUIRE(classify(conflict_newer, in) == write_dest);
        return true;
    }

    auto test_resume_and_rename() -> bool
    {
        auto in = existing(50, 100);
        in.state_file_usable = true;
        REQUIRE(classify(conflict_resume, in) == resume_dest);
        in.state_file_usable = false;
        REQUIRE(classify(conflict_resume, in) == write_dest);
        in.dest_size = 100;
        REQUIRE(classify(conflict_resume, in) == write_dest);
        REQUIRE(classify(conflict_rename, existing()) == rename_dest);
        REQUIRE(classify(conflict_ask, existing()) == ask_user);
        REQUIRE(classify(conflict_ask, conflict_inputs{}) == write_dest);
        return true;
    }

    auto test_names() -> bool
    {
        REQUIRE(uniquify_name("/d/file.bin") == "/d/file (1).bin");
        REQUIRE(numbered_name("/d/file.bin", 2) == "/d/file (2).bin");
        REQUIRE(uniquify_name("file") == "file (1)");
        REQUIRE(uniquify_name("archive.tar.gz") == "archive (1).tar.gz");
        return true;
    }

    auto test_queued_destination_identity() -> bool
    {
        REQUIRE(same_queued_dest(true, "C:\\d\\File.bin", "c:/d/file.bin", true));
        REQUIRE(!same_queued_dest(true, "C:\\d\\File.bin", "c:/d/file.bin", false));
        REQUIRE(!same_queued_dest(false, "/r/File.bin", "/r/file.bin", true));
        #if defined(_WIN32)
        REQUIRE(same_queued_dest(true, "C:\\d\\File.bin", "c:/d/file.bin"));
        #else
        REQUIRE(!same_queued_dest(true, "C:\\d\\File.bin", "c:/d/file.bin"));
        #endif
        return true;
    }
}

int main()
{
    auto failed = 0;
    for (auto test : { test_overwrite_and_skip, test_newer, test_resume_and_rename,
                       test_names, test_queued_destination_identity })
        if (!test()) ++failed;
    std::puts(failed ? "FAILED" : "OK");
    return failed ? 1 : 0;
}
