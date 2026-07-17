// Guard: `pulp import-design --url <figma.com scene URL>` is not an importable
// path and must say so.
//
// Why this exists: the CLI help and docs used to advertise
// `--from figma --url 'https://figma.com/design/...'` as a happy path. It cannot
// work by construction — `fetch_url_to_file` is a bare unauthenticated `curl`
// and no credential flag exists — so a private file 403s and a public one yields
// the Figma web app's HTML shell, which then dies inside `choc::json::parse`
// with an unrelated-looking error. The docs now name the working lanes and the
// CLI rejects these URLs up front.
//
// Layered coverage, mirroring test_cli_import_design.cpp:
//   - Classifier-level: `is_figma_app_url` is header-only and self-contained
//     (figma_url.hpp), so these cases run in every lane without building the
//     full import pipeline.
//   - CLI-level (shell-out): runs the built binary and asserts exit code +
//     stderr naming the working lanes. Skips politely when the binary isn't
//     built so the classifier cases still gate the behavior.

#include <catch2/catch_test_macros.hpp>

#include "figma_url.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using pulp::import_design::figma_app_url_error;
using pulp::import_design::is_figma_app_url;

TEST_CASE("figma.com scene URLs are rejected by --url", "[import-design][figma-url]") {
    // The exact shapes the old help text and docs advertised.
    CHECK(is_figma_app_url("https://figma.com/design/abc123/My-Plugin"));
    CHECK(is_figma_app_url("https://www.figma.com/design/abc123/My-Plugin?node-id=1-2"));
    CHECK(is_figma_app_url("https://www.figma.com/file/abc123/Legacy-File"));
    CHECK(is_figma_app_url("https://www.figma.com/proto/abc123/Prototype"));
    CHECK(is_figma_app_url("http://figma.com/design/abc123/Plain-Http"));
}

TEST_CASE("non-scene URLs still fetch normally", "[import-design][figma-url]") {
    // --url genuinely works against any host serving design data directly, so
    // the guard must not become a blanket figma.com or generic-URL block.
    CHECK_FALSE(is_figma_app_url("https://v0.dev/t/abc123"));
    CHECK_FALSE(is_figma_app_url("http://localhost:8000/design.json"));
    CHECK_FALSE(is_figma_app_url("https://example.com/figma.com/design/x"));
    // Other figma.com paths are not the web app's scene route.
    CHECK_FALSE(is_figma_app_url("https://www.figma.com/community/plugin/123"));
    CHECK_FALSE(is_figma_app_url("https://api.figma.com/v1/files/abc123"));
    // Bare host with no path is not a scene URL.
    CHECK_FALSE(is_figma_app_url("https://figma.com/"));
    CHECK_FALSE(is_figma_app_url(""));
}

TEST_CASE("figma --url error names the lanes that work", "[import-design][figma-url]") {
    // A bare "failed to fetch" is what made the old behavior confusing. The
    // message has to route the user somewhere real.
    const std::string msg = figma_app_url_error();
    CHECK(msg.find("figma-plugin") != std::string::npos);
    CHECK(msg.find("--from fig") != std::string::npos);
    CHECK(msg.find("figma_rest_export.py") != std::string::npos);
    CHECK(msg.find("MCP") != std::string::npos);
}

namespace {

// Mirrors test_cli_import_design.cpp's locator: the C++ delegate is the real
// implementation of this command; the Rust front-end forwards to it.
fs::path pulp_binary() {
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) return fs::path(env);
    const auto build = fs::current_path() / "..";
    for (const auto& candidate : {
             build / "tools" / "cli" / "pulp-cpp",
             build / "tools" / "cli" / "pulp",
             build / "pulp",
         }) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return build / "tools" / "cli" / "pulp-cpp";
}

}  // namespace

TEST_CASE("CLI rejects a figma.com scene URL with an actionable error",
          "[import-design][figma-url][cli]") {
    const auto binary = pulp_binary();
    if (!fs::exists(binary)) {
        WARN("pulp CLI not built — skipping shell-out case (classifier cases still cover the rule)");
        return;
    }

    const fs::path err_file =
        fs::temp_directory_path() / "pulp-figma-url-guard.err";
    const std::string command = "\"" + binary.string() +
                                "\" import-design --from figma --url "
                                "'https://www.figma.com/design/abc123/Test' 2> \"" +
                                err_file.string() + "\"";
    const int status = std::system(command.c_str());
    CHECK(status != 0);  // must not report success for an impossible import

    std::string stderr_text;
    {
        std::ifstream f(err_file);
        REQUIRE(f.is_open());
        std::ostringstream ss;
        ss << f.rdbuf();
        stderr_text = ss.str();
    }
    // The point of the guard: not a bare curl failure, but a route forward.
    CHECK(stderr_text.find("cannot be imported with --url") != std::string::npos);
    CHECK(stderr_text.find("figma-plugin") != std::string::npos);
    std::error_code ec;
    fs::remove(err_file, ec);
}
