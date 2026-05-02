#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace pulp::platform;
namespace fs = std::filesystem;

#ifndef PULP_IMPORT_DESIGN_TOOL_PATH
#define PULP_IMPORT_DESIGN_TOOL_PATH ""
#endif

namespace {

fs::path tool_binary() {
    if (const char* env = std::getenv("PULP_IMPORT_DESIGN_TOOL_PATH"); env && *env) {
        return fs::path(env);
    }
    return fs::path(PULP_IMPORT_DESIGN_TOOL_PATH);
}

bool binary_exists() {
    const auto bin = tool_binary();
    return !bin.empty() && fs::exists(bin);
}

ProcessResult run_import_design(const std::vector<std::string>& args, int timeout_ms = 30000) {
    const auto bin = tool_binary();
    if (bin.empty() || !fs::exists(bin)) {
        ProcessResult r;
        r.exit_code = -1;
        r.stderr_output = "pulp-import-design binary not at " + bin.string();
        return r;
    }
    return exec(bin.string(), args, timeout_ms);
}

class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / (prefix + "-" + std::to_string(tick));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path path;
};

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    REQUIRE(f.is_open());
    f << text;
    REQUIRE(f.good());
}

std::string read_text(const fs::path& path) {
    std::ifstream f(path);
    REQUIRE(f.is_open());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("pulp-import-design reports help and argument diagnostics",
          "[cli][import-design][tool][issue-493]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    SECTION("help exits cleanly") {
        auto r = run_import_design({"--help"});
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.stdout_output.find("pulp import-design") != std::string::npos);
        REQUIRE(r.stdout_output.find("--from <source>") != std::string::npos);
    }

    SECTION("missing source is a usage error") {
        auto r = run_import_design({});
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code != 0);
        REQUIRE(r.stderr_output.find("--from <source> is required") != std::string::npos);
        REQUIRE(r.stdout_output.find("Usage:") != std::string::npos);
    }

    SECTION("unknown source is rejected before input parsing") {
        auto r = run_import_design({"--from", "not-a-source"});
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code != 0);
        REQUIRE(r.stderr_output.find("unknown source 'not-a-source'") != std::string::npos);
        REQUIRE(r.stderr_output.find("Valid sources") != std::string::npos);
    }
}

TEST_CASE("pulp-import-design export-tokens dry-run emits the built-in theme",
          "[cli][import-design][tool][issue-493]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    auto r = run_import_design({"--export-tokens", "--dry-run"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("\"bg\"") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"$type\": \"color\"") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"$type\": \"dimension\"") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"$type\": \"string\"") != std::string::npos);
}

TEST_CASE("pulp-import-design writes a web-compat Stitch import to nested outputs",
          "[cli][import-design][tool][issue-493]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-stitch");
    const auto input = tmp.path / "screen.html";
    const auto output = tmp.path / "generated" / "ui.js";
    const auto tokens = tmp.path / "generated" / "tokens.json";

    write_text(input,
               "<!DOCTYPE html><html><body>"
               "<main class=\"panel\"><h1>Gain</h1><button>Bypass</button></main>"
               "</body></html>");

    auto r = run_import_design({"--from", "stitch",
                                "--file", input.string(),
                                "--output", output.string(),
                                "--tokens", tokens.string(),
                                "--web-compat",
                                "--no-comments",
                                "--no-tokens"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(output));
    REQUIRE_FALSE(fs::exists(tokens));

    const auto js = read_text(output);
    REQUIRE(js.find("setTheme('dark')") != std::string::npos);
    REQUIRE(js.find("document.createElement") != std::string::npos);
    REQUIRE(js.find("document.body.appendChild") != std::string::npos);
    REQUIRE(r.stdout_output.find(output.string()) != std::string::npos);
}
