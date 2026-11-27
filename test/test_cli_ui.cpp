#include <catch2/catch_test_macros.hpp>

#include "tools/cli/cli_common.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path delegated_script;
std::vector<std::string> delegated_args;
int delegated_result = 0;

fs::path project_root_result;
std::string forwarded_pulp_command;
std::string forwarded_vellum_command;
std::vector<std::string> forwarded_args;
int forwarded_result = 0;
bool forwarded = false;
std::atomic<unsigned> scratch_counter{0};

struct ScratchRoot {
    fs::path path;
    explicit ScratchRoot(bool with_tool) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto serial = scratch_counter.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path() /
               ("pulp-test-cli-ui-root-" + std::to_string(stamp) + "-" + std::to_string(serial));
        fs::remove_all(path);
        fs::create_directories(path / "tools" / "ui-build");
        if (with_tool)
            std::ofstream(path / "tools" / "ui-build" / "ui_build.py") << "\n";
    }
    ~ScratchRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct ScopedOutput {
    std::ostringstream out;
    std::ostringstream err;
    std::streambuf* old_out = std::cout.rdbuf(out.rdbuf());
    std::streambuf* old_err = std::cerr.rdbuf(err.rdbuf());

    ~ScopedOutput() {
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
    }
};

void reset_delegate(int result) {
    delegated_script.clear();
    delegated_args.clear();
    delegated_result = result;
    forwarded = false;
    forwarded_pulp_command.clear();
    forwarded_vellum_command.clear();
    forwarded_args.clear();
    forwarded_result = result;
}

} // namespace

fs::path find_project_root() {
    return project_root_result;
}

int forward_to_vellum(const std::string& pulp_command, const std::string& vellum_command,
                      const std::vector<std::string>& args) {
    forwarded = true;
    forwarded_pulp_command = pulp_command;
    forwarded_vellum_command = vellum_command;
    forwarded_args = args;
    return forwarded_result;
}

int delegate_to_python_script(const fs::path& relative_script,
                              const std::vector<std::string>& args) {
    delegated_script = relative_script;
    delegated_args = args;
    return delegated_result;
}

TEST_CASE("pulp ui without a subcommand prints the complete usage contract", "[cli][ui]") {
    reset_delegate(99);
    ScopedOutput output;

    REQUIRE(cmd_ui({}) == 0);
    CHECK(output.out.str().find("Usage: pulp ui <build|check|lint> [options]") !=
          std::string::npos);
    CHECK(output.out.str().find("  lint   check semantic source and captured corpus output") !=
          std::string::npos);
    CHECK(output.out.str().find("  --manifest <file>  corpus manifest (lint only)") !=
          std::string::npos);
    CHECK(output.err.str().empty());
    CHECK(delegated_script.empty());
}

TEST_CASE("pulp ui forwards lint arguments and preserves delegate failures", "[cli][ui]") {
    reset_delegate(37);
    ScratchRoot root(/*with_tool=*/true);
    project_root_result = root.path;
    const std::vector<std::string> args{"lint",       "--source",     "native-ui/src",
                                        "--manifest", "capture.json", "--json"};

    REQUIRE(cmd_ui(args) == 37);
    CHECK(delegated_script == fs::path("tools/ui-build/ui_build.py"));
    CHECK(delegated_args == args);
}

TEST_CASE("pulp ui forwards to vellum ui outside a checkout that carries the tool", "[cli][ui]") {
    const std::vector<std::string> args{"build", "--source", "native-ui/src", "--out", "out"};

    SECTION("no project root") {
        reset_delegate(41);
        project_root_result.clear();
        REQUIRE(cmd_ui(args) == 41);
    }
    SECTION("a checkout without tools/ui-build") {
        reset_delegate(41);
        ScratchRoot root(/*with_tool=*/false);
        project_root_result = root.path;
        REQUIRE(cmd_ui(args) == 41);
    }
    CHECK(forwarded);
    CHECK(forwarded_pulp_command == "ui");
    CHECK(forwarded_vellum_command == "ui");
    CHECK(forwarded_args == args);
    CHECK(delegated_script.empty());
}
