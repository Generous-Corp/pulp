#include <catch2/catch_test_macros.hpp>

#include "tools/cli/cli_common.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path delegated_script;
std::vector<std::string> delegated_args;
int delegated_result = 0;

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
}

} // namespace

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
    const std::vector<std::string> args{"lint",       "--source",     "native-ui/src",
                                        "--manifest", "capture.json", "--json"};

    REQUIRE(cmd_ui(args) == 37);
    CHECK(delegated_script == fs::path("tools/ui-build/ui_build.py"));
    CHECK(delegated_args == args);
}
