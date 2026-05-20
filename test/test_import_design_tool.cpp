#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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

std::optional<std::string> read_env_var(const char* name) {
    if (const char* value = std::getenv(name); value) return std::string(value);
    return std::nullopt;
}

void set_env_var(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unset_env_var(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name), old_(read_env_var(name)) {
        set_env_var(name_.c_str(), value);
    }

    ~ScopedEnvVar() {
        if (old_) set_env_var(name_.c_str(), *old_);
        else unset_env_var(name_.c_str());
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    std::optional<std::string> old_;
};

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
        REQUIRE(r.stdout_output.find("--output <path>   Destination file for the primary artifact") != std::string::npos);
        REQUIRE(r.stdout_output.find("--emit {js|ir-json|cpp}") != std::string::npos);
        REQUIRE(r.stdout_output.find("--mode {live|baked}") != std::string::npos);
        REQUIRE(r.stdout_output.find("--snapshot-semantics {fail|warn|accept}") != std::string::npos);
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

TEST_CASE("pulp-import-design validates phase 0.5 import vocabulary",
          "[cli][import-design][tool][issue-493]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-flags");
    const auto input = tmp.path / "screen.html";
    write_text(input, "<!DOCTYPE html><html><body><main>Gain</main></body></html>");

    SECTION("missing and unknown emit values are clean usage errors") {
        auto missing = run_import_design({"--emit"});
        REQUIRE_FALSE(missing.timed_out);
        REQUIRE(missing.exit_code == 2);
        REQUIRE(missing.stderr_output.find("--emit requires a value") != std::string::npos);

        auto unknown = run_import_design({"--emit", "tokens",
                                          "--from", "stitch",
                                          "--file", input.string()});
        REQUIRE_FALSE(unknown.timed_out);
        REQUIRE(unknown.exit_code == 2);
        REQUIRE(unknown.stderr_output.find("unsupported --emit value") != std::string::npos);
    }

    SECTION("ir-json emit writes canonical DesignIR v1 with asset manifest") {
        const auto ir_input = tmp.path / "screen-ir.json";
        const auto ir_output = tmp.path / "screen-ir.out.json";
        write_text(ir_input, R"json({
            "type": "frame",
            "name": "Screen",
            "style": {
                "backgroundImage": "url(data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/2000/svg'/%3E)"
            },
            "children": [
                { "type": "text", "name": "Title", "content": "Gain" }
            ]
        })json");

        auto ir = run_import_design({"--from", "stitch",
                                     "--file", ir_input.string(),
                                     "--emit", "ir-json",
                                     "--output", ir_output.string()});
        REQUIRE_FALSE(ir.timed_out);
        REQUIRE(ir.exit_code == 0);
        const auto out = read_text(ir_output);
        REQUIRE(out.find("\"version\":1") != std::string::npos);
        REQUIRE(out.find("\"assetManifest\"") != std::string::npos);
        REQUIRE(out.find("\"content_hash\"") != std::string::npos);
        REQUIRE(out.find("\"image/svg+xml\"") != std::string::npos);
    }

    SECTION("ir-json emit blocks first network fetch without explicit consent") {
        const auto network_input = tmp.path / "network-ir.json";
        const auto ir_output = tmp.path / "network-ir.out.json";
        write_text(network_input, R"json({
            "type": "frame",
            "name": "Screen",
            "style": { "backgroundImage": "url(https://example.test/hero.png)" }
        })json");

        auto ir = run_import_design({"--from", "stitch",
                                     "--file", network_input.string(),
                                     "--emit", "ir-json",
                                     "--output", ir_output.string()});
        REQUIRE_FALSE(ir.timed_out);
        REQUIRE(ir.exit_code == 1);
        REQUIRE(ir.stderr_output.find("asset-network-fetch-disabled") != std::string::npos);
    }

#ifndef _WIN32
    SECTION("ir-json emit fetches allowed network assets through cache and verifies hashes") {
        const auto bin = tmp.path / "bin";
        const auto curl = bin / "curl";
        fs::create_directories(bin);
        write_text(curl,
                   "#!/bin/sh\n"
                   "out=''\n"
                   "while [ \"$#\" -gt 0 ]; do\n"
                   "  case \"$1\" in\n"
                   "    --output) shift; out=\"$1\" ;;\n"
                   "  esac\n"
                   "  shift\n"
                   "done\n"
                   "[ -n \"$out\" ] || exit 9\n"
                   "printf '%s' '<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"1\" height=\"1\"/></svg>' > \"$out\"\n");
        fs::permissions(curl,
                        fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::add);

        const auto url = std::string("https://example.test/icon.svg");
        const auto expected_hash =
            std::string("b131556c36b0323b4981999443b5b22f7e35032e604d152b2d6698fc072e88c1");
        const auto network_input = tmp.path / "network-ir-allowed.json";
        const auto ir_output = tmp.path / "network-ir-allowed.out.json";
        const auto cached_output = tmp.path / "network-ir-cached.out.json";
        const auto cache_dir = tmp.path / "asset-cache";
        write_text(network_input, R"json({
            "type": "frame",
            "name": "Screen",
            "style": { "backgroundImage": "url(https://example.test/icon.svg)" }
        })json");

        auto old_path = read_env_var("PATH").value_or("");
        ScopedEnvVar path_override("PATH", bin.string() + ":" + old_path);

        auto fetched = run_import_design({"--from", "stitch",
                                          "--file", network_input.string(),
                                          "--emit", "ir-json",
                                          "--output", ir_output.string(),
                                          "--allow-network-fetch",
                                          "--asset-cache", cache_dir.string(),
                                          "--asset-timeout-ms", "5000",
                                          "--asset-hash", url + "=" + expected_hash});
        REQUIRE_FALSE(fetched.timed_out);
        REQUIRE(fetched.exit_code == 0);
        const auto fetched_json = read_text(ir_output);
        REQUIRE(fetched_json.find(expected_hash) != std::string::npos);
        REQUIRE(fetched_json.find("\"mime\":\"image/svg+xml\"") != std::string::npos);
        REQUIRE(fetched_json.find("\"source_url\":\"" + url + "\"") != std::string::npos);
        REQUIRE(fetched.stderr_output.find("asset-hash-mismatch") == std::string::npos);

        fs::remove(curl);
        auto cached = run_import_design({"--from", "stitch",
                                         "--file", network_input.string(),
                                         "--emit", "ir-json",
                                         "--output", cached_output.string(),
                                         "--allow-network-fetch",
                                         "--asset-cache", cache_dir.string()});
        REQUIRE_FALSE(cached.timed_out);
        REQUIRE(cached.exit_code == 0);
        REQUIRE(read_text(cached_output).find(expected_hash) != std::string::npos);
        REQUIRE(cached.stderr_output.find("asset-fetcher-missing") == std::string::npos);
    }
#endif

    SECTION("asset option diagnostics are usage errors") {
        auto missing_timeout = run_import_design({"--asset-timeout-ms"});
        REQUIRE_FALSE(missing_timeout.timed_out);
        REQUIRE(missing_timeout.exit_code == 2);
        REQUIRE(missing_timeout.stderr_output.find("--asset-timeout-ms requires a value") != std::string::npos);

        auto bad_hash = run_import_design({"--asset-hash", "not-a-pair"});
        REQUIRE_FALSE(bad_hash.timed_out);
        REQUIRE(bad_hash.exit_code == 2);
        REQUIRE(bad_hash.stderr_output.find("--asset-hash requires") != std::string::npos);
    }

    SECTION("cpp emit target is accepted vocabulary but not implemented") {
        auto cpp = run_import_design({"--from", "stitch",
                                      "--file", input.string(),
                                      "--emit", "cpp"});
        REQUIRE_FALSE(cpp.timed_out);
        REQUIRE(cpp.exit_code == 2);
        REQUIRE(cpp.stderr_output.find("--emit cpp is reserved") != std::string::npos);
    }

    SECTION("mode and snapshot vocabulary reject unsupported values") {
        auto mode = run_import_design({"--from", "stitch",
                                       "--file", input.string(),
                                       "--mode", "preview"});
        REQUIRE_FALSE(mode.timed_out);
        REQUIRE(mode.exit_code == 2);
        REQUIRE(mode.stderr_output.find("unsupported --mode value") != std::string::npos);

        auto snapshot = run_import_design({"--from", "stitch",
                                           "--file", input.string(),
                                           "--snapshot-semantics", "maybe"});
        REQUIRE_FALSE(snapshot.timed_out);
        REQUIRE(snapshot.exit_code == 2);
        REQUIRE(snapshot.stderr_output.find("unsupported --snapshot-semantics value") != std::string::npos);

        auto baked = run_import_design({"--from", "stitch",
                                        "--file", input.string(),
                                        "--mode", "baked",
                                        "--snapshot-semantics", "warn"});
        REQUIRE_FALSE(baked.timed_out);
        REQUIRE(baked.exit_code == 2);
        REQUIRE(baked.stderr_output.find("--mode baked is reserved") != std::string::npos);
    }

    SECTION("legacy classnames emit vocabulary remains accepted") {
        const auto output = tmp.path / "generated" / "legacy.js";
        auto r = run_import_design({"--from", "stitch",
                                    "--file", input.string(),
                                    "--output", output.string(),
                                    "--emit", "classnames",
                                    "--no-comments",
                                    "--no-tokens"});
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 0);
        REQUIRE(fs::exists(output));
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
    const auto shortcuts = tmp.path / "generated" / "shortcuts.json";

    write_text(input,
               "<!DOCTYPE html><html><body>"
               "<main class=\"panel\"><h1>Gain</h1><button>Bypass</button></main>"
               "<script>window.addEventListener('keydown', function(e) {"
               "if ((e.metaKey || e.ctrlKey) && e.key === 's') save();"
               "});</script>"
               "</body></html>");

    auto r = run_import_design({"--from", "stitch",
                                "--file", input.string(),
                                "--output", output.string(),
                                "--emit", "js",
                                "--mode", "live",
                                "--snapshot-semantics", "warn",
                                "--tokens", tokens.string(),
                                "--web-compat",
                                "--no-default-shortcuts",
                                "--no-comments",
                                "--no-tokens"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(output));
    REQUIRE(fs::exists(shortcuts));
    REQUIRE_FALSE(fs::exists(tokens));

    const auto js = read_text(output);
    const auto shortcuts_json = read_text(shortcuts);
    REQUIRE(js.find("setTheme('dark')") != std::string::npos);
    REQUIRE(js.find("document.createElement") != std::string::npos);
    REQUIRE(js.find("document.body.appendChild") != std::string::npos);
    REQUIRE(shortcuts_json.find("\"key\": \"s\"") != std::string::npos);
    REQUIRE(r.stdout_output.find(output.string()) != std::string::npos);
}

TEST_CASE("pulp-import-design accepts literal file paths and rejects unsafe URLs",
          "[cli][import-design][tool][issue-493]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-shell-meta");
    const auto sentinel = tmp.path / "sentinel";

    SECTION("--file accepts legal shell-metacharacter path bytes literally") {
        const auto literal =
            tmp.path / "design (1) [draft]! $not-expanded.html";
        write_text(literal, "<div>literal path</div>");

        auto r = run_import_design({"--from", "stitch", "--file", literal.string(),
                                    "--dry-run"});

        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 0);
        REQUIRE_FALSE(fs::exists(sentinel));
    }

    SECTION("--file still rejects control characters") {
        const auto bad = (tmp.path / "design.html").string() + "\nnext";
        auto r = run_import_design({"--from", "stitch", "--file", bad});

        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find("--file contains control characters") != std::string::npos);
        REQUIRE_FALSE(fs::exists(sentinel));
    }

    SECTION("--url is rejected before curl is launched") {
        const std::vector<std::string> metas = {
            ";", "|", "<", ">", "$", "`", "'", "\"", "\\", "(", ")",
            "*", "[", "]", "{", "}", "!"
        };
        for (const auto& meta : metas) {
            INFO("url metacharacter: " << meta);
            const auto hostile = std::string("https://example.invalid/screen") + meta + "touch";
            auto r = run_import_design({"--from", "stitch", "--url", hostile});
            REQUIRE_FALSE(r.timed_out);
            REQUIRE(r.exit_code == 2);
            REQUIRE(r.stderr_output.find("--url contains shell metacharacters") != std::string::npos);
            REQUIRE_FALSE(fs::exists(sentinel));
        }
    }
}

#ifndef _WIN32
TEST_CASE("pulp-import-design URL fetch uses a unique temp file and argv-safe curl",
          "[cli][import-design][tool][issue-493]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-url-fetch");
    const auto bin = tmp.path / "bin";
    const auto curl = bin / "curl";
    const auto output = tmp.path / "generated" / "ui.js";
    fs::create_directories(bin);
    write_text(curl,
               "#!/bin/sh\n"
               "out=''\n"
               "while [ \"$#\" -gt 0 ]; do\n"
               "  case \"$1\" in\n"
               "    --output) shift; out=\"$1\" ;;\n"
               "  esac\n"
               "  shift\n"
               "done\n"
               "[ -n \"$out\" ] || exit 9\n"
               "printf '%s' '<!DOCTYPE html><html><body><main>Fetched Gain</main></body></html>' > \"$out\"\n");
    fs::permissions(curl,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add);

    auto old_path = read_env_var("PATH").value_or("");
    ScopedEnvVar path_override("PATH", bin.string() + ":" + old_path);
    auto r = run_import_design({"--from", "stitch",
                                "--url", "https://example.test/screen.html?node-id=1&mode=dev",
                                "--output", output.string(),
                                "--no-comments",
                                "--no-tokens"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(output));
    REQUIRE(r.stdout_output.find("Fetched https://example.test/screen.html?node-id=1&mode=dev") != std::string::npos);
    REQUIRE(r.stdout_output.find("pulp-import-design-") != std::string::npos);
    REQUIRE(r.stdout_output.find("pulp-import-fetched.tmp") == std::string::npos);
}
#endif

TEST_CASE("pulp-import-design debug report names the default bridge-native mode",
          "[cli][import-design][tool][issue-2439]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-debug");
    const auto input = tmp.path / "screen.html";
    const auto output = tmp.path / "generated" / "ui.js";
    const auto debug = tmp.path / "generated" / "debug.json";

    write_text(input,
               "<!DOCTYPE html><html><body>"
               "<main><h1>Gain</h1><button>Bypass</button></main>"
               "</body></html>");

    auto r = run_import_design({"--from", "stitch",
                                "--file", input.string(),
                                "--output", output.string(),
                                "--debug-output", debug.string(),
                                "--no-comments",
                                "--no-tokens"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(output));
    REQUIRE(fs::exists(debug));

    const auto report = read_text(debug);
    REQUIRE(report.find("\"mode\": \"bridge_native_js\"") != std::string::npos);
    REQUIRE(report.find("\"mode\": \"native\"") == std::string::npos);
}
