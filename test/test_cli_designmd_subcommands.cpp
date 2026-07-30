// Shell-out regressions for DESIGN.md parse-error handling in `pulp design`.

#include "test_cli_shellout_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = pulp_test_cli::unique_temp_dir("pulp-designmd-cli");

    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    file << content;
    REQUIRE(file.good());
}

std::string collision_designmd() {
    return
        "---\n"
        "name: Collision\n"
        "colors:\n"
        "  brand-primary: \"#111111\"\n"
        "  brand:\n"
        "    primary: \"#222222\"\n"
        "---\n";
}

bool cli_available() {
    return fs::exists(pulp_test_cli::pulp_binary());
}

void require_parse_error(const pulp::platform::ProcessResult& result) {
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 3);
    REQUIRE(result.stderr_output.find("[error] token-name-collision at ") !=
            std::string::npos);
}

} // namespace

TEST_CASE("pulp design lint and diff reject DESIGN.md parse errors before analysis",
          "[cli][designmd][parse-error][shellout]") {
    if (!cli_available()) {
        SUCCEED("skipped: pulp CLI not built");
        return;
    }

    TempDir temp;
    const auto invalid = temp.path / "invalid-DESIGN.md";
    const auto valid = temp.path / "valid-DESIGN.md";
    write_text(invalid, collision_designmd());
    write_text(valid,
               "---\n"
               "name: Valid\n"
               "colors:\n"
               "  primary: \"#123456\"\n"
               "---\n");

    SECTION("lint") {
        const auto result =
            pulp_test_cli::run_pulp({"design", "lint", invalid.string()});
        require_parse_error(result);
        REQUIRE(result.stdout_output.find("Lint summary:") == std::string::npos);
    }

    SECTION("diff") {
        const auto result = pulp_test_cli::run_pulp(
            {"design", "diff", valid.string(), invalid.string()});
        require_parse_error(result);
        REQUIRE(result.stdout_output.find("colors:") == std::string::npos);
        REQUIRE(result.stdout_output.find("regression:") == std::string::npos);
    }
}

TEST_CASE("pulp design manifest consumers reject partial DESIGN.md parse results",
          "[cli][designmd][parse-error][shellout]") {
    if (!cli_available()) {
        SUCCEED("skipped: pulp CLI not built");
        return;
    }

    TempDir temp;
    const auto invalid = temp.path / "invalid-DESIGN.md";
    const auto js = temp.path / "ui.js";
    write_text(invalid, collision_designmd());
    write_text(js, "el.style.color = 'var(--brand-primary)';\n");

    SECTION("compile") {
        const auto result = pulp_test_cli::run_pulp(
            {"design", "compile", "--design-md", invalid.string(),
             "--stdout", "--json"});
        require_parse_error(result);
        REQUIRE(result.stdout_output.find("\"manifest_version\"") ==
                std::string::npos);
    }

    SECTION("lint-adherence") {
        const auto result = pulp_test_cli::run_pulp(
            {"design", "lint-adherence", js.string(),
             "--design-md", invalid.string()});
        require_parse_error(result);
        REQUIRE(result.stdout_output.find("finding(s)") == std::string::npos);
    }
}
