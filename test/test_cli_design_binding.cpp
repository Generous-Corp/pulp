#include <catch2/catch_test_macros.hpp>

#include "tools/cli/design_binding.hpp"

namespace fs = std::filesystem;

using pulp::cli::DesignBindingInput;
using pulp::cli::resolve_design_binding;
using pulp::cli::design_binding_autobind_error;

TEST_CASE("CLI design binding uses current checkout defaults", "[cli][design]") {
    DesignBindingInput input;
    input.cwd_root = "/repo";

    auto result = resolve_design_binding(input);

    REQUIRE(result.ok);
    CHECK(result.root == fs::path("/repo"));
    CHECK(result.root_reason == "current checkout");
    CHECK(result.build_dir == fs::path("/repo/build"));
    CHECK(result.build_reason == "default build dir under selected root");
    CHECK(result.script_path == fs::path("/repo/examples/design-tool/design-tool-core.js"));
    CHECK(result.script_reason == "default design tool script");
}

TEST_CASE("CLI design binding supports CLI-adjacent build trees", "[cli][design]") {
    DesignBindingInput input;
    input.binary_root = "/sdk";
    input.binary_build_dir = "/sdk/build";

    auto result = resolve_design_binding(input);

    REQUIRE(result.ok);
    CHECK(result.root == fs::path("/sdk"));
    CHECK(result.root_reason == "CLI-adjacent build tree");
    CHECK(result.build_dir == fs::path("/sdk/build"));
    CHECK(result.build_reason == "CLI-adjacent build tree");
    CHECK(result.script_path == fs::path("/sdk/examples/design-tool/design-tool-core.js"));
}

TEST_CASE("CLI design binding honors explicit build dir and script", "[cli][design]") {
    DesignBindingInput input;
    input.build_dir = "/sdk/build";
    input.script_path = "/worktree/examples/design-tool/design-tool-core.js";
    input.script_root = "/worktree";
    input.build_dir_cache_root = "/worktree";
    input.build_dir_explicit = true;
    input.script_explicit = true;

    auto result = resolve_design_binding(input);

    REQUIRE(result.ok);
    CHECK(result.root == fs::path("/worktree"));
    CHECK(result.root_reason == "script path");
    CHECK(result.build_dir == fs::path("/sdk/build"));
    CHECK(result.build_reason == "explicit --build-dir");
    CHECK(result.script_path == fs::path("/worktree/examples/design-tool/design-tool-core.js"));
    CHECK(result.script_reason == "explicit --script");
}

TEST_CASE("CLI design binding treats positional script as script-root selector",
          "[cli][design][coverage]") {
    DesignBindingInput input;
    input.cwd_root = "/repo";
    input.script_path = "/worktree/ui/main.js";
    input.script_root = "/worktree";
    input.script_explicit = false;

    auto result = resolve_design_binding(input);

    REQUIRE(result.ok);
    CHECK(result.root == fs::path("/worktree"));
    CHECK(result.root_reason == "script path");
    CHECK(result.script_path == fs::path("/worktree/ui/main.js"));
    CHECK(result.script_reason == "positional script argument");
    CHECK(result.build_dir == fs::path("/worktree/build"));
}

TEST_CASE("CLI design binding keeps checkout root when script is in same tree",
          "[cli][design][coverage]") {
    DesignBindingInput input;
    input.cwd_root = "/repo";
    input.script_path = "/repo/ui/main.js";
    input.script_root = "/repo";

    auto result = resolve_design_binding(input);

    REQUIRE(result.ok);
    CHECK(result.root == fs::path("/repo"));
    CHECK(result.root_reason == "current checkout");
    CHECK(result.script_reason == "positional script argument");
}

TEST_CASE("CLI design binding can select root from explicit build cache",
          "[cli][design][coverage]") {
    DesignBindingInput input;
    input.build_dir = "/repo/build";
    input.build_dir_cache_root = "/repo";
    input.build_dir_explicit = true;

    auto result = resolve_design_binding(input);

    REQUIRE(result.ok);
    CHECK(result.root == fs::path("/repo"));
    CHECK(result.root_reason == "build dir cache");
    CHECK(result.build_dir == fs::path("/repo/build"));
    CHECK(result.build_reason == "explicit --build-dir");
}

TEST_CASE("CLI design binding defaults build dir when binary root differs",
          "[cli][design][coverage]") {
    DesignBindingInput input;
    input.cwd_root = "/repo";
    input.binary_root = "/sdk";
    input.binary_build_dir = "/sdk/build";

    auto result = resolve_design_binding(input);

    REQUIRE(result.ok);
    CHECK(result.root == fs::path("/repo"));
    CHECK(result.build_dir == fs::path("/repo/build"));
    CHECK(result.build_reason == "default build dir under selected root");
}

TEST_CASE("CLI design binding fails clearly without a checkout or build tree", "[cli][design]") {
    DesignBindingInput input;

    auto result = resolve_design_binding(input);

    REQUIRE_FALSE(result.ok);
    CHECK(result.error == design_binding_autobind_error());
}

TEST_CASE("CLI design binding rejects mismatched explicit build dirs", "[cli][design]") {
    DesignBindingInput input;
    input.build_dir = "/sdk/build";
    input.script_path = "/worktree/examples/design-tool/design-tool-core.js";
    input.script_root = "/worktree";
    input.build_dir_cache_root = "/other-repo";
    input.build_dir_explicit = true;
    input.script_explicit = true;

    auto result = resolve_design_binding(input);

    REQUIRE_FALSE(result.ok);
    CHECK(result.error.find("Use a matching --build-dir") != std::string::npos);
}
