#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
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
        REQUIRE(r.stdout_output.find("Precompiled React JSX runtime bundle") != std::string::npos);
        REQUIRE(r.stdout_output.find("baked emits IR or C++ artifacts") != std::string::npos);
        REQUIRE(r.stdout_output.find("Built-in default is --mode live --emit js") != std::string::npos);
        REQUIRE(r.stdout_output.find("import_design.default_mode") != std::string::npos);
        REQUIRE(r.stdout_output.find("PULP_IMPORT_DESIGN_DEFAULT_EMIT") != std::string::npos);
        REQUIRE(r.stdout_output.find("CLI dispatch reserved") == std::string::npos);
        REQUIRE(r.stdout_output.find("baked reserved for future imports") == std::string::npos);
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
          "[cli][import-design][tool][issue-493][network]") {
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

        const auto url = std::string("https://example.test/icon.svg?v=1");
        const auto expected_hash =
            std::string("b131556c36b0323b4981999443b5b22f7e35032e604d152b2d6698fc072e88c1");
        const auto network_input = tmp.path / "network-ir-allowed.json";
        const auto ir_output = tmp.path / "network-ir-allowed.out.json";
        const auto cached_output = tmp.path / "network-ir-cached.out.json";
        const auto cache_dir = tmp.path / "asset-cache";
        write_text(network_input, R"json({
            "type": "frame",
            "name": "Screen",
            "style": { "backgroundImage": "url(https://example.test/icon.svg?v=1)" }
        })json");

        auto old_path = read_env_var("PATH").value_or("");
        ScopedEnvVar path_override("PATH", bin.string() + ":" + old_path);

        auto fetched = run_import_design({"--from", "stitch",
                                          "--file", network_input.string(),
                                          "--emit", "ir-json",
                                          "--output", ir_output.string(),
                                          "--allow-network-fetch",
                                          "--asset-cache", cache_dir.string(),
                                          "--asset-timeout-ms", "30000",
                                          "--asset-hash", url + "=" + expected_hash},
                                         60000);
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
                                         "--asset-cache", cache_dir.string()});
        REQUIRE_FALSE(cached.timed_out);
        REQUIRE(cached.exit_code == 0);
        REQUIRE(read_text(cached_output).find(expected_hash) != std::string::npos);
        REQUIRE(cached.stderr_output.find("asset-fetcher-missing") == std::string::npos);

        const auto cached_mismatch_output = tmp.path / "network-ir-cached-mismatch.out.json";
        auto cached_mismatch = run_import_design({"--from", "stitch",
                                                  "--file", network_input.string(),
                                                  "--emit", "ir-json",
                                                  "--output", cached_mismatch_output.string(),
                                                  "--asset-cache", cache_dir.string(),
                                                  "--asset-hash", url + "=bad"});
        REQUIRE_FALSE(cached_mismatch.timed_out);
        REQUIRE(cached_mismatch.exit_code == 1);
        REQUIRE(cached_mismatch.stderr_output.find("asset-hash-mismatch") != std::string::npos);
    }

    SECTION("ir-json emit resolves relative assets from the source URL") {
        const auto bin = tmp.path / "url-bin";
        const auto curl = bin / "curl";
        const auto fetched_url_log = tmp.path / "fetched-asset-url.txt";
        fs::create_directories(bin);
        write_text(curl,
                   "#!/bin/sh\n"
                   "out=''\n"
                   "url=''\n"
                   "while [ \"$#\" -gt 0 ]; do\n"
                   "  case \"$1\" in\n"
                   "    --output) shift; out=\"$1\" ;;\n"
                   "    http://*|https://*) url=\"$1\" ;;\n"
                   "  esac\n"
                   "  shift\n"
                   "done\n"
                   "[ -n \"$out\" ] || exit 9\n"
                   "case \"$url\" in\n"
                   "  *screen.json)\n"
                   "    printf '%s' '{\"type\":\"frame\",\"name\":\"Remote\",\"children\":[{\"type\":\"image\",\"name\":\"Hero\",\"src\":\"assets/icon.svg\"}]}' > \"$out\"\n"
                   "    ;;\n"
                   "  *assets/icon.svg)\n"
                   "    printf '%s' '<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"1\" height=\"1\"/></svg>' > \"$out\"\n"
                   "    [ -n \"$PULP_FAKE_CURL_LOG\" ] && printf '%s' \"$url\" > \"$PULP_FAKE_CURL_LOG\"\n"
                   "    ;;\n"
                   "  *) exit 8 ;;\n"
                   "esac\n");
        fs::permissions(curl,
                        fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::add);

        auto old_path = read_env_var("PATH").value_or("");
        ScopedEnvVar path_override("PATH", bin.string() + ":" + old_path);
        ScopedEnvVar log_override("PULP_FAKE_CURL_LOG", fetched_url_log.string());

        const auto ir_output = tmp.path / "relative-url.out.json";
        const auto cache_dir = tmp.path / "relative-url-cache";
        const auto source_url = std::string("https://example.test/screens/screen.json");
        const auto asset_url = std::string("https://example.test/screens/assets/icon.svg");
        auto fetched = run_import_design({"--from", "stitch",
                                          "--url", source_url,
                                          "--emit", "ir-json",
                                          "--output", ir_output.string(),
                                          "--allow-network-fetch",
                                          "--asset-cache", cache_dir.string(),
                                          "--asset-timeout-ms", "30000"},
                                         60000);
        REQUIRE_FALSE(fetched.timed_out);
        REQUIRE(fetched.exit_code == 0);
        const auto ir_json = read_text(ir_output);
        REQUIRE(ir_json.find("\"sourceFile\":\"" + source_url + "\"") != std::string::npos);
        REQUIRE(ir_json.find("\"original_uri\":\"assets/icon.svg\"") != std::string::npos);
        REQUIRE(ir_json.find("\"source_url\":\"" + asset_url + "\"") != std::string::npos);
        REQUIRE(ir_json.find("\"src\":\"assets/icon.svg\"") != std::string::npos);
        REQUIRE(ir_json.find("\"srcAssetId\":\"asset-") != std::string::npos);
        REQUIRE(read_text(fetched_url_log) == asset_url);
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

    SECTION("cpp emit writes a baked C++ source and header pair") {
        const auto cpp_input = tmp.path / "cpp-screen.json";
        const auto cpp_output = tmp.path / "generated" / "imported_ui.cpp";
        const auto header_output = tmp.path / "generated" / "imported_ui.hpp";
        write_text(cpp_input, R"json({
            "type": "frame",
            "name": "Panel",
            "stableAnchorId": "panel-root",
            "style": { "width": 240, "height": 120, "backgroundColor": "#112233" },
            "tokens": {
                "colors": { "bg.primary": "#112233" },
                "dimensions": { "panel.width": 240 }
            },
            "children": [
                {
                    "type": "frame",
                    "name": "Header",
                    "stableAnchorId": "header",
                    "style": { "width": 240, "height": 32 },
                    "children": [
                        { "type": "text", "name": "Title", "stableAnchorId": "title", "content": "Gain" }
                    ]
                }
            ]
        })json");

        auto cpp = run_import_design({"--from", "stitch",
                                      "--file", cpp_input.string(),
                                      "--mode", "baked",
                                      "--emit", "cpp",
                                      "--output", cpp_output.string()});
        REQUIRE_FALSE(cpp.timed_out);
        REQUIRE(cpp.exit_code == 0);
        REQUIRE(fs::exists(cpp_output));
        REQUIRE(fs::exists(header_output));
        const auto source = read_text(cpp_output);
        const auto header = read_text(header_output);
        REQUIRE(source.find("#include \"imported_ui.hpp\"") != std::string::npos);
        REQUIRE(source.find("namespace tokens") != std::string::npos);
        REQUIRE(source.find("build_header") != std::string::npos);
        REQUIRE(source.find("build_native_view_tree") == std::string::npos);
        REQUIRE(header.find("build_imported_ui") != std::string::npos);
        REQUIRE(header.find("bake_asset_manifest") != std::string::npos);
        REQUIRE(cpp.stdout_output.find(cpp_output.string()) != std::string::npos);

        auto missing_mode = run_import_design({"--from", "stitch",
                                               "--file", cpp_input.string(),
                                               "--emit", "cpp",
                                               "--output", (tmp.path / "missing-mode.cpp").string()});
        REQUIRE_FALSE(missing_mode.timed_out);
        REQUIRE(missing_mode.exit_code == 2);
        REQUIRE(missing_mode.stderr_output.find("--emit cpp requires --mode baked") != std::string::npos);
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
        REQUIRE(baked.stderr_output.find("--mode baked requires --emit ir-json or --emit cpp") != std::string::npos);
    }

    SECTION("persistent import-design defaults can select baked DesignIR") {
        const auto home = tmp.path / "pulp-home-baked-default";
        fs::create_directories(home);
        write_text(home / "config.toml",
                   "[import_design]\n"
                   "default_mode = \"baked\"\n");
        ScopedEnvVar pulp_home("PULP_HOME", home.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");

        const auto baked_output = tmp.path / "config-default.out.json";
        auto baked = run_import_design({"--from", "stitch",
                                        "--file", input.string(),
                                        "--output", baked_output.string()});
        REQUIRE_FALSE(baked.timed_out);
        REQUIRE(baked.exit_code == 0);
        REQUIRE(read_text(baked_output).find("\"version\":1") != std::string::npos);
        REQUIRE(baked.stdout_output.find("DesignIR v1") != std::string::npos);

        const auto live_output = tmp.path / "config-default-live.js";
        auto live_override = run_import_design({"--from", "stitch",
                                                "--file", input.string(),
                                                "--mode", "live",
                                                "--emit", "js",
                                                "--output", live_output.string()});
        REQUIRE_FALSE(live_override.timed_out);
        REQUIRE(live_override.exit_code == 0);
        REQUIRE(read_text(live_output).find("\"version\":1") == std::string::npos);
    }

    SECTION("environment import-design defaults override config defaults") {
        const auto home = tmp.path / "pulp-home-env-default";
        fs::create_directories(home);
        write_text(home / "config.toml",
                   "[import_design]\n"
                   "default_mode = \"live\"\n"
                   "default_emit = \"js\"\n");
        ScopedEnvVar pulp_home("PULP_HOME", home.string());
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "baked");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "cpp");

        const auto cpp_output = tmp.path / "env-default.cpp";
        const auto header_output = tmp.path / "env-default.hpp";
        auto cpp = run_import_design({"--from", "stitch",
                                      "--file", input.string(),
                                      "--output", cpp_output.string()});
        REQUIRE_FALSE(cpp.timed_out);
        REQUIRE(cpp.exit_code == 0);
        REQUIRE(fs::exists(cpp_output));
        REQUIRE(fs::exists(header_output));
        REQUIRE(cpp.stdout_output.find("env-default.cpp") != std::string::npos);
    }

    SECTION("invalid import-design default preferences are usage errors") {
        ScopedEnvVar mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE", "frozen");
        ScopedEnvVar emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT", "");
        auto bad_mode = run_import_design({"--from", "stitch",
                                           "--file", input.string(),
                                           "--output", (tmp.path / "bad-mode.js").string()});
        REQUIRE_FALSE(bad_mode.timed_out);
        REQUIRE(bad_mode.exit_code == 2);
        REQUIRE(bad_mode.stderr_output.find("invalid import-design default mode") != std::string::npos);
        REQUIRE(bad_mode.stderr_output.find("PULP_IMPORT_DESIGN_DEFAULT_MODE") != std::string::npos);
    }

    SECTION("jsx live mode writes the precompiled bundle verbatim") {
        const auto jsx = tmp.path / "live.bundle.js";
        const auto output = tmp.path / "live-ui.js";
        const std::string bundle =
            "/* precompiled JSX runtime bundle */\n"
            "(function(){ globalThis.__pulpLiveBundle = true; })();\n";
        write_text(jsx, bundle);

        auto live = run_import_design({"--from", "jsx",
                                       "--file", jsx.string(),
                                       "--output", output.string()});
        REQUIRE_FALSE(live.timed_out);
        REQUIRE(live.exit_code == 0);
        REQUIRE(read_text(output) == bundle);
        REQUIRE(live.stdout_output.find("JSX live bundle") != std::string::npos);

        auto validate = run_import_design({"--from", "jsx",
                                           "--file", jsx.string(),
                                           "--output", (tmp.path / "validate-live-ui.js").string(),
                                           "--validate"});
        REQUIRE_FALSE(validate.timed_out);
        REQUIRE(validate.exit_code == 2);
        REQUIRE(validate.stderr_output.find("writes the precompiled bundle verbatim") != std::string::npos);
        REQUIRE(validate.stderr_output.find("--mode baked --emit ir-json|cpp") != std::string::npos);

        const auto debug_output = tmp.path / "live-debug.json";
        auto debug = run_import_design({"--from", "jsx",
                                        "--file", jsx.string(),
                                        "--output", (tmp.path / "debug-live-ui.js").string(),
                                        "--debug-output", debug_output.string()});
        REQUIRE_FALSE(debug.timed_out);
        REQUIRE(debug.exit_code == 2);
        REQUIRE_FALSE(fs::exists(debug_output));
        REQUIRE(debug.stderr_output.find("does not support --validate, --reference, --diff, or --debug") != std::string::npos);
    }

    SECTION("jsx baked snapshots fail by default on dynamic APIs and can warn or accept") {
        const auto jsx = tmp.path / "dynamic.bundle.js";
        const auto fail_output = tmp.path / "dynamic-fail.ir.json";
        const auto warn_output = tmp.path / "dynamic-warn.ir.json";
        const auto accept_output = tmp.path / "dynamic-accept.ir.json";
        write_text(jsx,
                   "(function(){\n"
                   "  var root = document.getElementById('root') || document.body;\n"
                   "  root.setAttribute('data-stamp', String(Date.now()));\n"
                   "  for (var i = 0; i < 12; ++i) {\n"
                   "    var el = document.createElement('div');\n"
                   "    el.textContent = 'Phase 15 snapshot ' + i;\n"
                   "    root.appendChild(el);\n"
                   "  }\n"
                   "})();\n");

        auto fail = run_import_design({"--from", "jsx",
                                       "--file", jsx.string(),
                                       "--mode", "baked",
                                       "--emit", "ir-json",
                                       "--output", fail_output.string()});
        REQUIRE_FALSE(fail.timed_out);
        REQUIRE(fail.exit_code == 2);
        REQUIRE(fail.stderr_output.find("JSX baked snapshot uses dynamic APIs") != std::string::npos);
        REQUIRE_FALSE(fs::exists(fail_output));

        auto warn = run_import_design({"--from", "jsx",
                                       "--file", jsx.string(),
                                       "--mode", "baked",
                                       "--emit", "ir-json",
                                       "--snapshot-semantics", "warn",
                                       "--output", warn_output.string()});
        REQUIRE_FALSE(warn.timed_out);
        REQUIRE(warn.exit_code == 0);
        const auto warn_json = read_text(warn_output);
        REQUIRE(warn_json.find("\"source\":\"jsx\"") != std::string::npos);
        REQUIRE(warn_json.find("\"capture_method\":\"runtime_snapshot\"") != std::string::npos);
        REQUIRE(warn_json.find("\"source_adapter\":\"jsx-runtime\"") != std::string::npos);
        REQUIRE(warn_json.find("\"snapshot-dynamic-api\"") != std::string::npos);
        REQUIRE(warn_json.find("\"snapshotSemantics\":\"warn\"") != std::string::npos);
        REQUIRE(warn_json.find("Phase 15 snapshot") != std::string::npos);
        REQUIRE(warn_json.find("\"imported_at\"") != std::string::npos);

        auto accept = run_import_design({"--from", "jsx",
                                         "--file", jsx.string(),
                                         "--mode", "baked",
                                         "--emit", "ir-json",
                                         "--snapshot-semantics", "accept",
                                         "--output", accept_output.string()});
        REQUIRE_FALSE(accept.timed_out);
        REQUIRE(accept.exit_code == 0);
        const auto accept_json = read_text(accept_output);
        REQUIRE(accept_json.find("\"source\":\"jsx\"") != std::string::npos);
        REQUIRE(accept_json.find("\"capture_method\":\"runtime_snapshot\"") != std::string::npos);
        REQUIRE(accept_json.find("\"snapshotSemantics\":\"accept\"") != std::string::npos);
        REQUIRE(accept_json.find("Phase 15 snapshot") != std::string::npos);
        REQUIRE(accept_json.find("\"snapshot-dynamic-api\"") == std::string::npos);
    }

    SECTION("jsx baked snapshots fail instead of serializing fallback shells") {
        const auto jsx = tmp.path / "fallback.bundle.js";
        const auto output = tmp.path / "fallback.ir.json";
        write_text(jsx,
                   "(function(){ globalThis.__pulpPhase15NoMount = true; })();\n"
                   "// padding so parse_jsx_react treats this as a runtime bundle "
                   "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n");

        auto result = run_import_design({"--from", "jsx",
                                         "--file", jsx.string(),
                                         "--mode", "baked",
                                         "--emit", "ir-json",
                                         "--snapshot-semantics", "accept",
                                         "--output", output.string()});
        REQUIRE_FALSE(result.timed_out);
        REQUIRE(result.exit_code == 1);
        REQUIRE(result.stderr_output.find("JSX baked runtime snapshot failed") != std::string::npos);
        REQUIRE_FALSE(fs::exists(output));
    }

    SECTION("designmd warning diagnostics are printed once") {
        const auto designmd = tmp.path / "DESIGN.md";
        const auto output = tmp.path / "designmd.ir.json";
        write_text(designmd,
                   "---\n"
                   "name: Synthetic\n"
                   "colors:\n"
                   "  bad: \"not-a-color\"\n"
                   "---\n");

        auto result = run_import_design({"--from", "designmd",
                                         "--file", designmd.string(),
                                         "--emit", "ir-json",
                                         "--output", output.string()});
        REQUIRE_FALSE(result.timed_out);
        REQUIRE(result.exit_code == 0);
        REQUIRE(fs::exists(output));
        REQUIRE(count_occurrences(result.stderr_output, "color-shape") == 1);
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

TEST_CASE("pulp-import-design handles literal file paths and rejects unsafe URLs",
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

    SECTION("--file accepts literal filesystem punctuation without shell interpretation") {
        const auto input = tmp.path / "design (1) [final]! $&;'*.html";
        const auto output = tmp.path / "literal.js";
        write_text(input, "<!doctype html><h1>Literal path</h1>");

        auto r = run_import_design({"--from", "stitch",
                                    "--file", input.string(),
                                    "--output", output.string(),
                                    "--no-comments",
                                    "--no-tokens"});
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 0);
        REQUIRE(fs::exists(output));
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
          "[cli][import-design][tool][issue-493][network]") {
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
                                "--no-tokens"},
                               60000);

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
