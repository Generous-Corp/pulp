#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>
#include <miniz.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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

// Like run_import_design but with a chosen working directory, so
// default-output-filename behavior (e.g. theme.css written to cwd) can be
// asserted without writing into the test runner's directory. The binary path
// is resolved absolute since the child's cwd moves.
ProcessResult run_import_design_in(const fs::path& cwd,
                                   const std::vector<std::string>& args,
                                   int timeout_ms = 30000) {
    const auto bin = tool_binary();
    if (bin.empty() || !fs::exists(bin)) {
        ProcessResult r;
        r.exit_code = -1;
        r.stderr_output = "pulp-import-design binary not at " + bin.string();
        return r;
    }
    ProcessOptions opts;
    opts.working_directory = cwd.string();
    opts.timeout_ms = timeout_ms;
    return ChildProcess::run(fs::absolute(bin).string(), args, opts);
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

bool has_float_call_argument_near(std::string_view haystack,
                                  std::string_view call,
                                  float expected,
                                  float tolerance) {
    size_t start = 0;
    while ((start = haystack.find(call, start)) != std::string_view::npos) {
        const auto value_start = start + call.size();
        const auto value_end = haystack.find("f);", value_start);
        if (value_end == std::string_view::npos) return false;
        const auto value = std::stof(std::string(haystack.substr(
            value_start, value_end - value_start)));
        if (std::fabs(value - expected) <= tolerance)
            return true;
        start = value_end + 3;
    }
    return false;
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
        REQUIRE(r.stdout_output.find("--emit {js|ir-json|cpp|swiftui}") != std::string::npos);
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
        const auto binding_output = tmp.path / "generated" / "imported_ui.bindings.json";
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
        REQUIRE(fs::exists(binding_output));
        const auto source = read_text(cpp_output);
        const auto header = read_text(header_output);
        const auto binding_manifest = read_text(binding_output);
        REQUIRE(source.find("#include \"imported_ui.hpp\"") != std::string::npos);
        REQUIRE(source.find("namespace tokens") != std::string::npos);
        REQUIRE(source.find("build_header") != std::string::npos);
        REQUIRE(source.find("build_native_view_tree") == std::string::npos);
        REQUIRE(header.find("build_imported_ui") != std::string::npos);
        REQUIRE(header.find("bake_asset_manifest") != std::string::npos);
        REQUIRE(binding_manifest.find("\"schema\": \"pulp-native-cpp-binding-manifest-v1\"") != std::string::npos);
        REQUIRE(cpp.stdout_output.find(cpp_output.string()) != std::string::npos);
        REQUIRE(cpp.stdout_output.find(binding_output.string()) != std::string::npos);

        auto missing_mode = run_import_design({"--from", "stitch",
                                               "--file", cpp_input.string(),
                                               "--emit", "cpp",
                                               "--output", (tmp.path / "missing-mode.cpp").string()});
        REQUIRE_FALSE(missing_mode.timed_out);
        REQUIRE(missing_mode.exit_code == 2);
        REQUIRE(missing_mode.stderr_output.find("--emit cpp requires --mode baked") != std::string::npos);
    }

    SECTION("cpp emit lowers a CSS background gradient to a runtime call") {
        // A style.backgroundGradient (the light hero panels + illustration fills
        // in real Figma imports) must lower to an apply_css_background_gradient
        // runtime call plus the helper include. Dropping it was the dominant
        // ELYSIUM dark/light parity gap. This lives in the always-built tool
        // test so the codegen path is covered even when the artifact-gated
        // pulp-test-design-import-cpp-codegen target is skipped.
        const auto grad_input = tmp.path / "gradient-envelope.json";
        const auto cpp_output = tmp.path / "generated" / "gradient_ui.cpp";
        write_text(grad_input, R"json({
            "version": 1,
            "source": "stitch",
            "capture_method": "gradient-emit-test",
            "source_adapter": "gradient-emit-test",
            "source_version": "1",
            "root": {
                "type": "frame",
                "name": "Hero",
                "stable_anchor_id": "hero",
                "style": {
                    "width": 320, "height": 200,
                    "backgroundColor": "#1c1d1d",
                    "backgroundGradient": "linear-gradient(to bottom, #e4edf6, #b7c8db)"
                }
            },
            "tokens": { "colors": {}, "dimensions": {}, "strings": {} },
            "assetManifest": { "version": 1, "assets": [] }
        })json");

        auto cpp = run_import_design({"--from", "stitch",
                                      "--file", grad_input.string(),
                                      "--mode", "baked",
                                      "--emit", "cpp",
                                      "--output", cpp_output.string()});
        REQUIRE_FALSE(cpp.timed_out);
        REQUIRE(cpp.exit_code == 0);
        REQUIRE(fs::exists(cpp_output));
        const auto source = read_text(cpp_output);
        REQUIRE(source.find("pulp::view::apply_css_background_gradient(*")
                != std::string::npos);
        REQUIRE(source.find("linear-gradient(to bottom, #e4edf6, #b7c8db)")
                != std::string::npos);
        REQUIRE(source.find("#include <pulp/view/css_gradient.hpp>")
                != std::string::npos);
    }

    SECTION("cpp emit accepts serialized DesignIR envelopes with typed widget metadata") {
        const auto ir_input = tmp.path / "typed-envelope.json";
        const auto cpp_output = tmp.path / "generated" / "typed_ui.cpp";
        const auto header_output = tmp.path / "generated" / "typed_ui.hpp";
        const auto binding_output = tmp.path / "generated" / "typed_ui.bindings.json";
        write_text(ir_input, R"json({
            "version": 1,
            "source": "jsx",
            "capture_method": "phase-c-one-knob-route-overlay",
            "source_adapter": "native-cpp-import-execution-validation",
            "source_version": "phase-c",
            "root": {
                "type": "frame",
                "name": "Root",
                "stable_anchor_id": "root",
                "style": { "width": 80, "height": 90 },
                "layout": { "direction": "column" },
                "children": [
                    {
                        "type": "frame",
                        "name": "freq_meter",
                        "stable_anchor_id": "pr_2c",
                        "style": { "width": 52, "height": 52, "borderColor": "#ff6b35" },
                        "audioWidget": "knob",
                        "label": "freq",
                        "min": 0,
                        "max": 1,
                        "default": 0.35,
                        "attributes": {
                            "value": "0.35",
                            "pulpRouteId": "chainer.knob.0.osc_freq",
                            "pulpRouteType": "native_cpp",
                            "pulpSourceFamily": "Knob",
                            "pulpParamKey": "osc_freq",
                            "pulpBindingModule": "OSC",
                            "pulpBindingParam": "freq",
                            "pulpEventContract": "onChange:set_param:osc_freq",
                            "pulpGestureContract": "rotary_drag:begin/update/end",
                            "pulpStyleTokens": "C.orange",
                            "pulpDefaultValueSource": "phase_c_initial_value_fallback"
                        }
                    }
                ]
            },
            "tokens": {
                "colors": { "chainer.orange": "#ff6b35" },
                "dimensions": {},
                "strings": {}
            },
            "assetManifest": { "version": 1, "assets": [] }
        })json");

        auto cpp = run_import_design({"--from", "stitch",
                                      "--file", ir_input.string(),
                                      "--mode", "baked",
                                      "--emit", "cpp",
                                      "--output", cpp_output.string()});
        REQUIRE_FALSE(cpp.timed_out);
        REQUIRE(cpp.exit_code == 0);
        REQUIRE(fs::exists(cpp_output));
        REQUIRE(fs::exists(header_output));
        REQUIRE(fs::exists(binding_output));

        const auto source = read_text(cpp_output);
        const auto binding_manifest = read_text(binding_output);
        REQUIRE(source.find("std::make_unique<pulp::view::Knob>()") != std::string::npos);
        REQUIRE(source.find("->set_anchor_id(\"pr_2c\");") != std::string::npos);
        REQUIRE(source.find("->set_label(\"freq\");") != std::string::npos);
        REQUIRE(source.find("->set_value(/* imported static param value */ 0.35f);") != std::string::npos);
        REQUIRE(source.find("tokens::kChainerOrange") != std::string::npos);
        REQUIRE(binding_manifest.find("\"id\": \"chainer.knob.0.osc_freq\"") != std::string::npos);
        REQUIRE(binding_manifest.find("\"native_primitive\": \"knob\"") != std::string::npos);
        REQUIRE(binding_manifest.find("\"param_key\": \"osc_freq\"") != std::string::npos);
        REQUIRE(binding_manifest.find("\"style_tokens\": \"C.orange\"") != std::string::npos);
        REQUIRE(binding_manifest.find("\"default_value_source\": \"phase_c_initial_value_fallback\"") != std::string::npos);
    }

    SECTION("baked emit accepts source-less canonical DesignIR envelopes") {
        const auto ir_input = tmp.path / "source-less-envelope.json";
        const auto ir_output = tmp.path / "generated" / "source_less.ir.json";
        write_text(ir_input, R"json({
            "version": 1,
            "root": {
                "type": "frame",
                "name": "Source Less Root",
                "style": { "width": 80, "height": 90 },
                "children": [
                    { "type": "text", "name": "Label", "content": "Hello" }
                ]
            },
            "assetManifest": { "version": 1, "assets": [] }
        })json");

        auto ir_json = run_import_design({"--from", "stitch",
                                          "--file", ir_input.string(),
                                          "--mode", "baked",
                                          "--emit", "ir-json",
                                          "--output", ir_output.string()});
        REQUIRE_FALSE(ir_json.timed_out);
        INFO("stderr: " << ir_json.stderr_output);
        INFO("stdout: " << ir_json.stdout_output);
        REQUIRE(ir_json.exit_code == 0);
        REQUIRE(fs::exists(ir_output));
        const auto emitted = read_text(ir_output);
        REQUIRE(emitted.find("Source Less Root") != std::string::npos);
        REQUIRE(emitted.find("\"assetManifest\"") != std::string::npos);
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
        REQUIRE(baked.stderr_output.find("--mode baked requires --emit ir-json, --emit cpp, or --emit swiftui") != std::string::npos);
    }

    SECTION("persistent import-design defaults can select baked DesignIR") {
        const auto home = tmp.path / "pulp-home-baked-default";
        fs::create_directories(home);
        write_text(home / "config.toml",
                   "[import_design]\n"
                   "default_mode = 'baked'\n");
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

// ──────────────────────────────────────────────────────────────────────────
// .pulp.zip auto-unpack
//
// The Pulp Figma plugin ships its "Export to Pulp" as a `.pulp.zip` that
// contains scene.pulp.json + assets/*.png. Before this fix the CLI read
// the input file via std::ifstream and silently truncated at the first
// NUL byte in the ZIP header — the user had to manually `unzip` before
// running the import. This test pins the auto-unpack contract by
// assembling a minimal .pulp.zip from C++ (one entry, scene.pulp.json,
// holding a valid figma-plugin envelope with a recognised Pulp / Knob
// instance) and asserting the CLI parses it without an intermediate
// unzip step.

namespace {

bool make_pulp_zip(
    const fs::path& zip_path,
    const std::string& scene_json,
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& extra_entries = {}) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zip_path.string().c_str(), 0)) return false;
    bool ok = mz_zip_writer_add_mem(
        &zip,
        "scene.pulp.json",
        scene_json.data(),
        scene_json.size(),
        MZ_DEFAULT_COMPRESSION);
    if (!ok) {
        mz_zip_writer_end(&zip);
        return false;
    }
    for (const auto& [name, bytes] : extra_entries) {
        ok = mz_zip_writer_add_mem(
            &zip,
            name.c_str(),
            bytes.data(),
            bytes.size(),
            MZ_DEFAULT_COMPRESSION);
        if (!ok) {
            mz_zip_writer_end(&zip);
            return false;
        }
    }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        return false;
    }
    mz_zip_writer_end(&zip);
    return true;
}

std::vector<std::uint8_t> tiny_png_bytes() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
        0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9c, 0x63, 0xf8, 0x0f, 0x04, 0x00,
        0x09, 0xfb, 0x03, 0xfd, 0xa7, 0xe9, 0x81, 0x86,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
        0xae, 0x42, 0x60, 0x82,
    };
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void append_png_chunk(std::vector<std::uint8_t>& out,
                      const char type[4],
                      const std::vector<std::uint8_t>& data) {
    append_be32(out, static_cast<std::uint32_t>(data.size()));
    const size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const auto crc = mz_crc32(MZ_CRC32_INIT,
                              out.data() + crc_start,
                              out.size() - crc_start);
    append_be32(out, static_cast<std::uint32_t>(crc));
}

std::vector<std::uint8_t> transparent_bleed_png_bytes() {
    constexpr std::uint32_t width = 6;
    constexpr std::uint32_t height = 4;
    std::vector<std::uint8_t> raw;
    raw.reserve(height * (1 + width * 4));
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // PNG filter type 0.
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool opaque_core = x >= 2 && x <= 4 && y >= 1 && y <= 2;
            raw.push_back(opaque_core ? 0x20 : 0x00);
            raw.push_back(opaque_core ? 0x80 : 0x00);
            raw.push_back(opaque_core ? 0xff : 0x00);
            raw.push_back(opaque_core ? 0xff : 0x00);
        }
    }

    mz_ulong compressed_size = compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<std::uint8_t> compressed(compressed_size);
    REQUIRE(mz_compress(compressed.data(), &compressed_size, raw.data(), raw.size()) == MZ_OK);
    compressed.resize(compressed_size);

    std::vector<std::uint8_t> png = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    };
    std::vector<std::uint8_t> ihdr;
    append_be32(ihdr, width);
    append_be32(ihdr, height);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // RGBA
    ihdr.push_back(0); // compression
    ihdr.push_back(0); // filter
    ihdr.push_back(0); // interlace
    append_png_chunk(png, "IHDR", ihdr);
    append_png_chunk(png, "IDAT", compressed);
    append_png_chunk(png, "IEND", {});
    return png;
}

}  // namespace

TEST_CASE("pulp-import-design auto-unpacks .pulp.zip Figma-plugin exports",
          "[cli][import-design][tool][figma-plugin][issue-47]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip");
    const auto zip = tmp.path / "smoke.pulp.zip";
    const auto output = tmp.path / "ui.js";

    // Minimal figma-plugin envelope with a Pulp / Knob recognised by the
    // audio_widget="knob" path. The full envelope shape is
    // documented in tools/figma-plugin/schema/figma-plugin-export-v1.json
    // and exercised end-to-end by test/test_design_import_codegen.cpp's
    // "[figma-plugin][phase-3]" cases. Here we only care that the CLI
    // unpacks the zip and round-trips the IR through to the codegen.
    const std::string envelope = R"JSON({
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": {
            "adapter": "figma-plugin",
            "version": "0.1.0",
            "source_uri": "test://issue-47-fixture"
        },
        "asset_manifest": { "version": 1, "assets": [] },
        "diagnostics": [],
        "root": {
            "type": "frame",
            "name": "TestRoot",
            "figma_node_id": "1:1",
            "style": { "width": 200, "height": 200 },
            "layout": { "direction": "column", "gap": 8,
                        "paddingTop": 16, "paddingRight": 16,
                        "paddingBottom": 16, "paddingLeft": 16 },
            "children": [
                {
                    "type": "frame",
                    "name": "Cutoff Knob",
                    "figma_node_id": "1:2",
                    "audio_widget": "knob",
                    "label": "Cutoff",
                    "min": 20,
                    "max": 20000,
                    "default": 880,
                    "attributes": { "units": "Hz", "binding": "filter.cutoff_hz" },
                    "style": { "width": 56, "height": 56 },
                    "layout": { "direction": "column" },
                    "children": []
                }
            ]
        }
    })JSON";

    REQUIRE(make_pulp_zip(zip, envelope));
    REQUIRE(fs::exists(zip));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string()});

    REQUIRE_FALSE(r.timed_out);
    INFO("stderr: " << r.stderr_output);
    INFO("stdout: " << r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(output));

    // Visibility of the auto-unpack on stdout — both confirms the path
    // ran and gives the user a breadcrumb if they later want to inspect
    // the extracted contents.
    REQUIRE(r.stdout_output.find("Unpacked ") != std::string::npos);
    REQUIRE(r.stdout_output.find(".pulp.zip") != std::string::npos);

    // The CLI should have emitted a native createKnob call with the
    // designer-set range / value. If this assertion fails after a
    // future codegen refactor, update the expected pattern rather than
    // weakening the contract: the IR must survive the zip round-trip.
    const auto js = read_text(output);
    REQUIRE(js.find("createKnob('Cutoff_Knob") != std::string::npos);
    REQUIRE(js.find("setValue('Cutoff_Knob") != std::string::npos);
    REQUIRE(js.find("880") != std::string::npos);
}

TEST_CASE("pulp-import-design persists .pulp.zip assets beside generated output",
          "[cli][import-design][tool][figma-plugin][issue-47][assets]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-assets");
    const auto zip = tmp.path / "with-asset.pulp.zip";
    const auto output = tmp.path / "nested" / "ui.js";
    const fs::path sidecar(output.string() + ".assets");

    const std::string envelope = R"JSON({
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": {
            "adapter": "figma-plugin",
            "version": "0.1.0",
            "source_uri": "test://issue-47-asset-fixture"
        },
        "asset_manifest": { "version": 1, "assets": [
            { "asset_id": "hero", "local_path": "assets/tiny.png", "mime": "image/png" }
        ] },
        "diagnostics": [],
        "root": {
            "type": "frame",
            "name": "TestRoot",
            "figma_node_id": "1:1",
            "style": { "width": 64, "height": 64 },
            "children": [
                {
                    "type": "image",
                    "name": "Hero",
                    "figma_node_id": "1:2",
                    "asset_ref": "hero",
                    "style": { "width": 32, "height": 32 },
                    "children": []
                }
            ]
        }
    })JSON";

    REQUIRE(make_pulp_zip(zip, envelope, {{"assets/tiny.png", tiny_png_bytes()}}));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string()});

    REQUIRE_FALSE(r.timed_out);
    INFO("stderr: " << r.stderr_output);
    INFO("stdout: " << r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(output));
    REQUIRE(fs::exists(sidecar / "scene.pulp.json"));
    REQUIRE(fs::exists(sidecar / "assets" / "tiny.png"));
    REQUIRE(fs::exists(sidecar / ".pulp-import-design-sidecar-v1"));
    REQUIRE(r.stdout_output.find("assets staged for generated output") != std::string::npos);
    REQUIRE(r.stdout_output.find("Persisted ZIP assets") != std::string::npos);

    const auto js = read_text(output);
    REQUIRE(js.find("setImageSource('Hero") != std::string::npos);
    REQUIRE(js.find("ui.js.assets") != std::string::npos);
    REQUIRE(js.find("assets/tiny.png") != std::string::npos);
}

TEST_CASE("pulp-import-design replaces only marked .pulp.zip asset sidecars on success",
          "[cli][import-design][tool][figma-plugin][issue-47][assets]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-assets-replace");
    const auto zip = tmp.path / "with-asset.pulp.zip";
    const auto output = tmp.path / "nested" / "ui.js";
    const fs::path sidecar(output.string() + ".assets");
    write_text(sidecar / ".pulp-import-design-sidecar-v1", "managed-by=pulp-import-design\n");
    write_text(sidecar / "sentinel.txt", "old marked sidecar");

    const std::string envelope = R"JSON({
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": { "adapter": "figma-plugin", "version": "0.1.0" },
        "asset_manifest": { "version": 1, "assets": [
            { "asset_id": "hero", "local_path": "assets/tiny.png", "mime": "image/png" }
        ] },
        "diagnostics": [],
        "root": {
            "type": "frame",
            "name": "TestRoot",
            "figma_node_id": "1:1",
            "style": { "width": 64, "height": 64 },
            "children": [
                {
                    "type": "image",
                    "name": "Hero",
                    "figma_node_id": "1:2",
                    "asset_ref": "hero",
                    "style": { "width": 32, "height": 32 },
                    "children": []
                }
            ]
        }
    })JSON";

    REQUIRE(make_pulp_zip(zip, envelope, {{"assets/tiny.png", tiny_png_bytes()}}));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string()});

    REQUIRE_FALSE(r.timed_out);
    INFO("stderr: " << r.stderr_output);
    INFO("stdout: " << r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(output));
    REQUIRE(fs::exists(sidecar / ".pulp-import-design-sidecar-v1"));
    REQUIRE(fs::exists(sidecar / "scene.pulp.json"));
    REQUIRE(fs::exists(sidecar / "assets" / "tiny.png"));
    REQUIRE_FALSE(fs::exists(sidecar / "sentinel.txt"));

    for (const auto& entry : fs::directory_iterator(tmp.path)) {
        REQUIRE(entry.path().filename().string().find(".backup") == std::string::npos);
    }
}

TEST_CASE("pulp-import-design dry-run .pulp.zip extraction stays temporary",
          "[cli][import-design][tool][figma-plugin][issue-47][assets]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-dry-run");
    const auto zip = tmp.path / "with-asset.pulp.zip";
    const auto output = tmp.path / "nested" / "ui.js";
    const fs::path sidecar(output.string() + ".assets");

    const std::string envelope = R"JSON({
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": { "adapter": "figma-plugin", "version": "0.1.0" },
        "asset_manifest": { "version": 1, "assets": [
            { "asset_id": "hero", "local_path": "assets/tiny.png", "mime": "image/png" }
        ] },
        "diagnostics": [],
        "root": {
            "type": "frame",
            "name": "TestRoot",
            "figma_node_id": "1:1",
            "style": { "width": 64, "height": 64 },
            "children": []
        }
    })JSON";

    REQUIRE(make_pulp_zip(zip, envelope, {{"assets/tiny.png", tiny_png_bytes()}}));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string(),
                                "--dry-run"});

    REQUIRE_FALSE(r.timed_out);
    INFO("stderr: " << r.stderr_output);
    INFO("stdout: " << r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Unpacked ") != std::string::npos);
    REQUIRE(r.stdout_output.find("assets staged for generated output") == std::string::npos);
    REQUIRE(r.stdout_output.find("createCol('root'") != std::string::npos);
    REQUIRE_FALSE(fs::exists(output));
    REQUIRE_FALSE(fs::exists(sidecar));
}

TEST_CASE("pulp-import-design baked ir-json .pulp.zip uses the figma-plugin parser",
          "[cli][import-design][tool][figma-plugin][issue-47][assets][baked]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-baked-ir");
    const auto zip = tmp.path / "with-asset.pulp.zip";
    const auto output = tmp.path / "nested" / "ui.ir.json";
    const fs::path sidecar(output.string() + ".assets");

    const std::string envelope = R"JSON({
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": { "adapter": "figma-plugin", "version": "0.1.0" },
        "asset_manifest": { "version": 1, "assets": [
            { "asset_id": "hero", "local_path": "assets/tiny.png", "mime": "image/png" }
        ] },
        "diagnostics": [],
        "root": {
            "type": "frame",
            "name": "TestRoot",
            "figma_node_id": "1:1",
            "style": { "width": 64, "height": 64 },
            "children": [
                {
                    "type": "frame",
                    "name": "Cutoff Knob",
                    "figma_node_id": "1:2",
                    "audio_widget": "knob",
                    "asset_ref": "hero",
                    "label": "Cutoff",
                    "min": 20,
                    "max": 20000,
                    "default": 880,
                    "attributes": { "binding": "filter.cutoff_hz" },
                    "style": { "width": 56, "height": 56 },
                    "children": []
                }
            ]
        }
    })JSON";

    REQUIRE(make_pulp_zip(zip, envelope, {{"assets/tiny.png", tiny_png_bytes()}}));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string(),
                                "--mode", "baked",
                                "--emit", "ir-json"});

    REQUIRE_FALSE(r.timed_out);
    INFO("stderr: " << r.stderr_output);
    INFO("stdout: " << r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(output));
    REQUIRE(fs::exists(sidecar / ".pulp-import-design-sidecar-v1"));
    REQUIRE(fs::exists(sidecar / "assets" / "tiny.png"));

    const auto ir = read_text(output);
    REQUIRE(ir.find("\"source\":\"figma-plugin\"") != std::string::npos);
    REQUIRE(ir.find("Cutoff Knob") != std::string::npos);
    REQUIRE(ir.find("\"assetManifest\"") != std::string::npos);
    REQUIRE(ir.find("assets/tiny.png") != std::string::npos);
}

TEST_CASE("pulp-import-design keeps .pulp.zip assets beside resolved C++ source output",
          "[cli][import-design][tool][figma-plugin][issue-47][assets][baked]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-cpp-sidecar");
    const auto zip = tmp.path / "valid.pulp.zip";
    const auto output_root = tmp.path / "cpp-out";
    const auto source = output_root / "imported_ui.cpp";
    const auto header = output_root / "imported_ui.hpp";
    const auto binding_manifest = output_root / "imported_ui.bindings.json";
    const fs::path sidecar(source.string() + ".assets");

    const std::string envelope = R"JSON({
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": { "adapter": "figma-plugin", "version": "0.1.0" },
        "asset_manifest": { "version": 1, "assets": [
            { "asset_id": "hero", "local_path": "assets/tiny.png", "mime": "image/png" }
        ] },
        "diagnostics": [],
        "root": {
            "type": "frame",
            "name": "TestRoot",
            "figma_node_id": "1:1",
            "style": { "width": 64, "height": 64 },
            "children": [
                {
                    "type": "image",
                    "name": "Hero",
                    "figma_node_id": "1:2",
                    "asset_ref": "hero",
                    "style": { "width": 32, "height": 32 },
                    "children": []
                }
            ]
        }
    })JSON";

    REQUIRE(make_pulp_zip(zip, envelope, {{"assets/tiny.png", tiny_png_bytes()}}));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output_root.string(),
                                "--mode", "baked",
                                "--emit", "cpp"});

    REQUIRE_FALSE(r.timed_out);
    INFO("stderr: " << r.stderr_output);
    INFO("stdout: " << r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(source));
    REQUIRE(fs::exists(header));
    REQUIRE(fs::exists(binding_manifest));
    REQUIRE(fs::exists(sidecar / ".pulp-import-design-sidecar-v1"));
    REQUIRE(fs::exists(sidecar / "assets" / "tiny.png"));
    REQUIRE_FALSE(fs::exists(fs::path(output_root.string() + ".assets")));
}

TEST_CASE("pulp-import-design enriches .pulp.zip image metadata before baked C++ emit",
          "[cli][import-design][tool][figma-plugin][assets][baked][fidelity]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-cpp-image-metadata");
    const auto zip = tmp.path / "metadata.pulp.zip";
    const auto output_root = tmp.path / "cpp-out";
    const auto source = output_root / "imported_ui.cpp";

    const std::string envelope = R"JSON({
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": { "adapter": "figma-plugin", "version": "0.1.0" },
        "asset_manifest": { "version": 1, "assets": [
            { "asset_id": "hero", "local_path": "assets/tiny.png", "mime": "image/png" }
        ] },
        "diagnostics": [],
        "root": {
            "type": "frame",
            "name": "TestRoot",
            "figma_node_id": "1:1",
            "style": { "width": 64, "height": 64 },
            "children": [
                {
                    "type": "image",
                    "name": "Bleed Sprite",
                    "figma_node_id": "1:2",
                    "asset_ref": "hero",
                    "style": {
                        "position": "absolute",
                        "left": 5,
                        "top": 7,
                        "width": 10,
                        "height": 20,
                        "render_bounds": { "w": 20, "h": 20, "dx": -5, "dy": 0 }
                    },
                    "children": []
                }
            ]
        }
    })JSON";

    REQUIRE(make_pulp_zip(zip, envelope, {{"assets/tiny.png", transparent_bleed_png_bytes()}}));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output_root.string(),
                                "--mode", "baked",
                                "--emit", "cpp"});

    REQUIRE_FALSE(r.timed_out);
    INFO("stderr: " << r.stderr_output);
    INFO("stdout: " << r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(source));

    const auto cpp = read_text(source);
    REQUIRE(cpp.find("->set_image_source(") != std::string::npos);
    REQUIRE(cpp.find("_image_flex.preferred_width = 20.0f;") != std::string::npos);
    REQUIRE(cpp.find("_image_flex.preferred_height = 13.33333f;") != std::string::npos);
    REQUIRE(cpp.find("_image_flex.dim_height = {13.33333f, pulp::view::DimensionUnit::px};")
            != std::string::npos);
    REQUIRE(has_float_call_argument_near(cpp, "->set_left(", -1.666667f, 1e-5f));
    REQUIRE(has_float_call_argument_near(cpp, "->set_top(", 10.33333f, 1e-5f));
}

TEST_CASE("pulp-import-design rejects .pulp.zip with no scene.pulp.json",
          "[cli][import-design][tool][figma-plugin][issue-47]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-empty");
    const auto zip = tmp.path / "empty.pulp.zip";
    const auto output = tmp.path / "ui.js";

    // Build a zip whose only entry is `note.txt` — i.e. no scene
    // envelope. The CLI must surface a clear error rather than silently
    // succeed with empty content.
    {
        mz_zip_archive z{};
        REQUIRE(mz_zip_writer_init_file(&z, zip.string().c_str(), 0));
        REQUIRE(mz_zip_writer_add_mem(&z, "note.txt", "hi", 2, MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_finalize_archive(&z));
        mz_zip_writer_end(&z);
    }

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string()});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("scene.pulp.json") != std::string::npos);
    REQUIRE_FALSE(fs::exists(output));
}

TEST_CASE("pulp-import-design keeps existing .pulp.zip asset sidecar on extraction failure",
          "[cli][import-design][tool][figma-plugin][issue-47][assets]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-asset-fail");
    const auto zip = tmp.path / "empty.pulp.zip";
    const auto output = tmp.path / "nested" / "ui.js";
    const fs::path sidecar(output.string() + ".assets");
    const auto sentinel = sidecar / "sentinel.txt";
    write_text(sentinel, "old sidecar");

    {
        mz_zip_archive z{};
        REQUIRE(mz_zip_writer_init_file(&z, zip.string().c_str(), 0));
        REQUIRE(mz_zip_writer_add_mem(&z, "note.txt", "hi", 2, MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_finalize_archive(&z));
        mz_zip_writer_end(&z);
    }

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string()});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("scene.pulp.json") != std::string::npos);
    REQUIRE_FALSE(fs::exists(output));
    REQUIRE(fs::exists(sentinel));
    REQUIRE(read_text(sentinel) == "old sidecar");
}

TEST_CASE("pulp-import-design preserves existing output and sidecar when staged .pulp.zip parse fails",
          "[cli][import-design][tool][figma-plugin][issue-47][assets]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-parse-fail");
    const auto zip = tmp.path / "malformed.pulp.zip";
    const auto output = tmp.path / "nested" / "ui.js";
    const fs::path sidecar(output.string() + ".assets");
    const auto sentinel = sidecar / "sentinel.txt";
    write_text(output, "old ui");
    write_text(sentinel, "old sidecar");

    REQUIRE(make_pulp_zip(zip, "{ not valid json"));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string()});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(read_text(output) == "old ui");
    REQUIRE(fs::exists(sentinel));
    REQUIRE(read_text(sentinel) == "old sidecar");
}

TEST_CASE("pulp-import-design refuses to replace an unmarked .pulp.zip asset sidecar",
          "[cli][import-design][tool][figma-plugin][issue-47][assets]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-unmarked-sidecar");
    const auto zip = tmp.path / "with-asset.pulp.zip";
    const auto output = tmp.path / "nested" / "ui.js";
    const fs::path sidecar(output.string() + ".assets");
    const auto sentinel = sidecar / "sentinel.txt";
    write_text(sentinel, "user sidecar");

    const std::string envelope = R"JSON({
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": { "adapter": "figma-plugin", "version": "0.1.0" },
        "asset_manifest": { "version": 1, "assets": [] },
        "diagnostics": [],
        "root": {
            "type": "frame",
            "name": "TestRoot",
            "figma_node_id": "1:1",
            "style": { "width": 64, "height": 64 },
            "children": []
        }
    })JSON";
    REQUIRE(make_pulp_zip(zip, envelope));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string()});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("unmarked asset sidecar") != std::string::npos);
    REQUIRE_FALSE(fs::exists(output));
    REQUIRE(read_text(sentinel) == "user sidecar");
}

TEST_CASE("pulp-import-design restores marked .pulp.zip sidecar when output write fails",
          "[cli][import-design][tool][figma-plugin][issue-47][assets]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-write-fail");
    const auto zip = tmp.path / "valid.pulp.zip";
    const auto output = tmp.path / "nested" / "ui.js";
    const fs::path sidecar(output.string() + ".assets");
    const auto sentinel = sidecar / "sentinel.txt";
    write_text(sidecar / ".pulp-import-design-sidecar-v1", "managed-by=pulp-import-design\n");
    write_text(sentinel, "old marked sidecar");
    fs::create_directories(output);

    const std::string envelope = R"JSON({
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": { "adapter": "figma-plugin", "version": "0.1.0" },
        "asset_manifest": { "version": 1, "assets": [] },
        "diagnostics": [],
        "root": {
            "type": "frame",
            "name": "TestRoot",
            "figma_node_id": "1:1",
            "style": { "width": 64, "height": 64 },
            "children": []
        }
    })JSON";
    REQUIRE(make_pulp_zip(zip, envelope));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string()});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("cannot write file over directory") != std::string::npos);
    REQUIRE(fs::is_directory(output));
    REQUIRE(fs::exists(sidecar / ".pulp-import-design-sidecar-v1"));
    REQUIRE(read_text(sentinel) == "old marked sidecar");
}

TEST_CASE("pulp-import-design rolls back .pulp.zip sidecar and C++ files as one transaction",
          "[cli][import-design][tool][figma-plugin][issue-47][assets]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-cpp-write-fail");
    const auto zip = tmp.path / "valid.pulp.zip";
    const auto output = tmp.path / "nested" / "imported_ui.hpp";
    const auto source = tmp.path / "nested" / "imported_ui.cpp";
    const auto header = output;
    const auto binding_manifest = tmp.path / "nested" / "imported_ui.bindings.json";
    const fs::path sidecar(source.string() + ".assets");
    const auto sentinel = sidecar / "sentinel.txt";
    write_text(header, "old header");
    write_text(binding_manifest, "old binding manifest");
    write_text(sidecar / ".pulp-import-design-sidecar-v1", "managed-by=pulp-import-design\n");
    write_text(sentinel, "old marked sidecar");
    fs::create_directories(source);

    const std::string envelope = R"JSON({
        "format_version": "2026.05-figma-plugin-v1",
        "parser_version": "0.1.0",
        "compat_schema_version": "0.3",
        "provenance": { "adapter": "figma-plugin", "version": "0.1.0" },
        "asset_manifest": { "version": 1, "assets": [] },
        "diagnostics": [],
        "root": {
            "type": "frame",
            "name": "TestRoot",
            "figma_node_id": "1:1",
            "style": { "width": 64, "height": 64 },
            "children": []
        }
    })JSON";
    REQUIRE(make_pulp_zip(zip, envelope));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string(),
                                "--mode", "baked",
                                "--emit", "cpp"});

    REQUIRE_FALSE(r.timed_out);
    INFO("stderr: " << r.stderr_output);
    INFO("stdout: " << r.stdout_output);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("cannot write file over directory") != std::string::npos);
    REQUIRE(fs::is_directory(source));
    REQUIRE(read_text(header) == "old header");
    REQUIRE(read_text(binding_manifest) == "old binding manifest");
    REQUIRE(fs::exists(sidecar / ".pulp-import-design-sidecar-v1"));
    REQUIRE(read_text(sentinel) == "old marked sidecar");
}

// ──────────────────────────────────────────────────────────────────────────
// .pulp.zip extractor hardening.
//
// These cases pin three zip-slip/zip-bomb gaps that the original guards
// missed:
//   (1) mz_zip_reader_get_filename truncates to the supplied buffer
//       size; a 2-KB entry name like
//         "<1020 safe chars>/../../../etc/passwd"
//       becomes "<1020 safe chars>/..." after truncation — sneaks past
//       the `..` substring check — and then extract_to_file is called
//       with the FULL untruncated name from the central directory.
//   (2) The path-prefix check only rejected entries starting with `/`
//       or `\`. Windows drive-letter (C:\...) and UNC (\\srv\share\...)
//       paths slipped through and would escape temp_dir on Windows.
//   (3) No cap on total / per-file uncompressed size or entry count, so
//       a zip-bomb could fill the temp filesystem.
//
// The fixtures below pin each guard with a hostile zip the CLI must
// refuse.

namespace {

// Convenience: build a zip with one (name, contents) entry. Used to
// construct hostile fixtures from inside the test.
bool make_zip_with_entry(const fs::path& zip_path,
                         const std::string& entry_name,
                         const std::string& contents) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zip_path.string().c_str(), 0)) return false;
    const bool ok = mz_zip_writer_add_mem(
        &zip,
        entry_name.c_str(),
        contents.data(),
        contents.size(),
        MZ_DEFAULT_COMPRESSION);
    if (!ok) { mz_zip_writer_end(&zip); return false; }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        return false;
    }
    mz_zip_writer_end(&zip);
    return true;
}

}  // namespace

TEST_CASE("pulp-import-design refuses .pulp.zip with oversized filename (issue-50 truncation bypass)",
          "[cli][import-design][tool][figma-plugin][issue-50]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-truncation");
    const auto zip = tmp.path / "evil.pulp.zip";
    const auto output = tmp.path / "ui.js";

    // Craft a name long enough to truncate inside the original 1024-byte
    // stack buffer (>= 1024 chars). The trailing `..` is what the
    // truncation would have lost. With the new probe-first check the
    // CLI must reject before ever calling extract_to_file.
    const std::string oversized_name(1100, 'a');
    REQUIRE(make_zip_with_entry(zip, oversized_name + "/../../../etc/passwd", "x"));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", output.string()});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    INFO("stderr: " << r.stderr_output);
    REQUIRE(r.stderr_output.find("oversized filename") != std::string::npos);
    REQUIRE_FALSE(fs::exists(output));
}

TEST_CASE("pulp-import-design refuses .pulp.zip with `..` in entry path",
          "[cli][import-design][tool][figma-plugin][issue-50]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-dotdot");
    const auto zip = tmp.path / "evil.pulp.zip";

    REQUIRE(make_zip_with_entry(zip, "subdir/../../../etc/passwd", "x"));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", (tmp.path / "ui.js").string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    INFO("stderr: " << r.stderr_output);
    REQUIRE(r.stderr_output.find("'..'") != std::string::npos);
    REQUIRE_FALSE(fs::exists(tmp.path / "ui.js"));
}

TEST_CASE("pulp-import-design refuses .pulp.zip with Windows drive-relative path",
          "[cli][import-design][tool][figma-plugin][issue-50]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-driveletter");
    const auto zip = tmp.path / "evil.pulp.zip";

    // C:foo is drive-relative — fs::path::is_absolute() on Linux says
    // false, so the explicit drive-letter guard is what catches it.
    REQUIRE(make_zip_with_entry(zip, "C:Windows/system32/evil.dll", "x"));

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", (tmp.path / "ui.js").string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    INFO("stderr: " << r.stderr_output);
    REQUIRE(r.stderr_output.find("drive-relative") != std::string::npos);
}

// NOTE: a "leading slash absolute path" fixture is intentionally omitted —
// miniz's writer (mz_zip_writer_add_mem) refuses entries beginning with
// `/`, so a hand-crafted ZIP would be required to exercise that specific
// path. The path-safety branch is already covered by the `..` test
// (substring guard) and the Windows drive-relative test (explicit
// drive-letter guard); the leading-slash fallback is defence-in-depth
// for non-portable archives we'd never produce ourselves.

TEST_CASE("pulp-import-design refuses .pulp.zip exceeding the file-count cap",
          "[cli][import-design][tool][figma-plugin][issue-50]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-design-tool-zip-count-cap");
    const auto zip = tmp.path / "many.pulp.zip";

    // Pack 10001 minimal entries — one over the kMaxFileCount cap.
    {
        mz_zip_archive z{};
        REQUIRE(mz_zip_writer_init_file(&z, zip.string().c_str(), 0));
        for (int i = 0; i < 10001; ++i) {
            const auto name = "f" + std::to_string(i) + ".bin";
            const char body = 'x';
            REQUIRE(mz_zip_writer_add_mem(&z, name.c_str(), &body, 1,
                                          MZ_DEFAULT_COMPRESSION));
        }
        REQUIRE(mz_zip_writer_finalize_archive(&z));
        mz_zip_writer_end(&z);
    }

    auto r = run_import_design({"--from", "figma-plugin",
                                "--file", zip.string(),
                                "--output", (tmp.path / "ui.js").string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    INFO("stderr: " << r.stderr_output);
    REQUIRE(r.stderr_output.find("entries (>") != std::string::npos);
}

// ── `--format css-variables` token export ────────────────────────────────
// export_css_variables is unit-covered in test_design_import_w3c_tokens.cpp.
// These exercise the CLI dispatch end-to-end through the standalone tool (true
// exit codes): the --format flag selects the body, the sidecar default flips
// to theme.css, and an unknown format fails loudly instead of silently W3C.

TEST_CASE("pulp-import-design --export-tokens --format css-variables writes theme.css",
          "[cli][import-design][tool][css]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-export-tokens-css");
    // No --tokens: the css-variables default leaf is theme.css, written to cwd.
    auto r = run_import_design_in(tmp.path, {"--export-tokens",
                                             "--format", "css-variables"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);

    // Default sidecar flipped to theme.css (NOT the W3C tokens.json default).
    REQUIRE(fs::exists(tmp.path / "theme.css"));
    REQUIRE_FALSE(fs::exists(tmp.path / "tokens.json"));

    const auto css = read_text(tmp.path / "theme.css");
    REQUIRE(css.find(":root {") != std::string::npos);
    REQUIRE(css.find("--") != std::string::npos);          // at least one custom prop
    REQUIRE(css.find("$value") == std::string::npos);      // not the W3C JSON shape

    // Stdout names the format so users notice the non-default path.
    REQUIRE(r.stdout_output.find("format=css-variables") != std::string::npos);
}

TEST_CASE("pulp-import-design --export-tokens defaults to W3C tokens.json",
          "[cli][import-design][tool][css]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-export-tokens-w3c");
    auto r = run_import_design_in(tmp.path, {"--export-tokens"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);

    // W3C remains the default: tokens.json, JSON shape, no theme.css.
    REQUIRE(fs::exists(tmp.path / "tokens.json"));
    REQUIRE_FALSE(fs::exists(tmp.path / "theme.css"));
    REQUIRE(read_text(tmp.path / "tokens.json").find("$value") != std::string::npos);
}

TEST_CASE("pulp-import-design --export-tokens --tokens honors an explicit css path",
          "[cli][import-design][tool][css]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-export-tokens-css-explicit");
    auto out = tmp.path / "custom-theme.css";
    auto r = run_import_design({"--export-tokens",
                                "--format", "css-variables",
                                "--tokens", out.string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(out));
    REQUIRE(read_text(out).find(":root {") != std::string::npos);
}

TEST_CASE("pulp-import-design rejects an unknown --format value",
          "[cli][import-design][tool][css]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-export-tokens-bogus");
    auto out = tmp.path / "out.txt";
    auto r = run_import_design({"--export-tokens",
                                "--format", "bogus-format",
                                "--tokens", out.string()});
    REQUIRE_FALSE(r.timed_out);
    // Config error (exit 2), not a silent W3C fallback.
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("unsupported --format") != std::string::npos);
    REQUIRE_FALSE(fs::exists(out));
}

// Tailwind formats are gated to --from designmd. They must be rejected (exit 2)
// rather than silently emitting W3C under the requested format name — both in
// --export-tokens (no designmd context) and on a non-designmd import source.
TEST_CASE("pulp-import-design --export-tokens rejects tailwind formats",
          "[cli][import-design][tool][css]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-export-tokens-tailwind");
    for (const char* fmt : {"tailwind", "json-tailwind", "css-tailwind"}) {
        auto out = tmp.path / (std::string("out-") + fmt);
        auto r = run_import_design({"--export-tokens",
                                    "--format", fmt,
                                    "--tokens", out.string()});
        INFO("format: " << fmt << " stderr: " << r.stderr_output);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find("designmd") != std::string::npos);
        REQUIRE_FALSE(fs::exists(out));  // no silent W3C write under a tailwind name
    }
}

TEST_CASE("pulp-import-design rejects tailwind format on a non-designmd source",
          "[cli][import-design][tool][css]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-import-tailwind-nondesignmd");
    // Minimal figma-plugin envelope; we never get to parse it — the format
    // gate fires right after source validation.
    auto scene = tmp.path / "scene.pulp.json";
    write_text(scene, R"({"format_version":"2026.05-figma-plugin-v1",)"
                      R"("provenance":{"adapter":"figma-plugin","version":"t",)"
                      R"("source_uri":"figma://x/1:1"},)"
                      R"("root":{"type":"frame","name":"Root","figma_node_id":"1:1"}})");
    auto r = run_import_design({"--from", "figma-plugin",
                               "--file", scene.string(),
                               "--output", (tmp.path / "ui.js").string(),
                               "--format", "tailwind"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("requires --from designmd") != std::string::npos);
}

// --validate's render backend selector. Skia is the faithful default (it
// composites file-backed images; CoreGraphics renders an image's filename
// placeholder). An unknown value is rejected up front.
TEST_CASE("pulp-import-design rejects an unknown --screenshot-backend",
          "[cli][import-design][tool][screenshot]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-screenshot-backend-bogus");
    auto scene = tmp.path / "scene.pulp.json";
    write_text(scene, R"({"format_version":"2026.05-figma-plugin-v1",)"
                      R"("provenance":{"adapter":"figma-plugin","version":"t",)"
                      R"("source_uri":"figma://x/1:1"},)"
                      R"("root":{"type":"frame","name":"Root","figma_node_id":"1:1"}})");
    auto r = run_import_design({"--from", "figma-plugin",
                               "--file", scene.string(),
                               "--output", (tmp.path / "ui.js").string(),
                               "--no-tokens", "--validate",
                               "--screenshot-backend", "bogus"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("--screenshot-backend must be") != std::string::npos);
}

TEST_CASE("pulp-import-design validates style selector values",
          "[cli][import-design][tool][style]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-style-selector-values");
    auto scene = tmp.path / "scene.pulp.json";
    write_text(scene, R"({"format_version":"2026.05-figma-plugin-v1",)"
                      R"("provenance":{"adapter":"figma-plugin","version":"t",)"
                      R"("source_uri":"figma://x/1:1"},)"
                      R"("root":{"type":"frame","name":"Root","figma_node_id":"1:1"}})");

    auto ok = run_import_design({"--from", "figma-plugin",
                                 "--file", scene.string(),
                                 "--output", (tmp.path / "ok.js").string(),
                                 "--no-tokens",
                                 "--knob-style", "auto",
                                 "--fader-style=skin",
                                 "--meter-style", "plain"});
    REQUIRE_FALSE(ok.timed_out);
    REQUIRE(ok.exit_code == 0);

    struct BadCase {
        std::vector<std::string> args;
        const char* error;
    };
    const std::vector<BadCase> cases = {
        {{"--knob-style", "chrome"}, "--knob-style must be"},
        {{"--fader-style=chrome"}, "--fader-style must be"},
        {{"--meter-style", "chrome"}, "--meter-style must be"},
    };

    for (const auto& c : cases) {
        auto args = std::vector<std::string>{"--from", "figma-plugin",
                                             "--file", scene.string(),
                                             "--output", (tmp.path / "bad.js").string(),
                                             "--no-tokens"};
        args.insert(args.end(), c.args.begin(), c.args.end());
        auto r = run_import_design(args);
        INFO(r.stderr_output);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.error) != std::string::npos);
    }
}

// `--emit swiftui` per-view theme naming. The theme artifact/type derive from
// the view name (<RootView>Theme[.swift]) so the view and theme are always
// distinct files (no clobber, even for --output PulpTheme.swift) and two
// imports never collide on a shared theme.
namespace {
const char* kMiniSwiftScene =
    R"({"format_version":"2026.05-figma-plugin-v1",)"
    R"("provenance":{"adapter":"figma-plugin","version":"t","source_uri":"figma://x/1:1"},)"
    R"("root":{"type":"frame","name":"Root","figma_node_id":"1:1"}})";
}

TEST_CASE("pulp-import-design --emit swiftui PulpTheme.swift output does not clobber the view",
          "[cli][import-design][tool][swiftui]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-swiftui-collision");
    auto scene = tmp.path / "scene.pulp.json";
    write_text(scene, kMiniSwiftScene);
    auto view_out = tmp.path / "PulpTheme.swift";
    auto r = run_import_design({"--from", "figma-plugin", "--file", scene.string(),
                                "--mode", "baked", "--emit", "swiftui",
                                "--output", view_out.string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);

    // PulpTheme.swift holds the VIEW (struct), not the theme enum; the theme
    // landed at the distinct PulpThemeTheme.swift.
    REQUIRE(fs::exists(view_out));
    const auto view = read_text(view_out);
    REQUIRE(view.find("struct PulpTheme") != std::string::npos);   // the view, not clobbered
    REQUIRE(view.find("enum") == std::string::npos);
    REQUIRE(fs::exists(tmp.path / "PulpThemeTheme.swift"));
}

TEST_CASE("pulp-import-design --emit swiftui gives each view a distinct theme (no clobber)",
          "[cli][import-design][tool][swiftui]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp-import-design not built"); return; }

    TempDir tmp("pulp-swiftui-multiview");
    auto scene = tmp.path / "scene.pulp.json";
    write_text(scene, kMiniSwiftScene);
    std::vector<std::string> base = {"--from", "figma-plugin", "--file", scene.string(),
                                     "--mode", "baked", "--emit", "swiftui"};
    auto foo = base; foo.push_back("--output"); foo.push_back((tmp.path / "FooView.swift").string());
    auto bar = base; bar.push_back("--output"); bar.push_back((tmp.path / "BarView.swift").string());
    REQUIRE(run_import_design(foo).exit_code == 0);
    REQUIRE(run_import_design(bar).exit_code == 0);

    // Two imports into one dir produce distinct view + theme files — the second
    // does not overwrite the first's theme.
    REQUIRE(fs::exists(tmp.path / "FooView.swift"));
    REQUIRE(fs::exists(tmp.path / "FooViewTheme.swift"));
    REQUIRE(fs::exists(tmp.path / "BarView.swift"));
    REQUIRE(fs::exists(tmp.path / "BarViewTheme.swift"));
    // Distinct theme enum names → no duplicate-symbol clash in one Swift target.
    REQUIRE(read_text(tmp.path / "FooViewTheme.swift").find("enum FooViewTheme") != std::string::npos);
    REQUIRE(read_text(tmp.path / "BarViewTheme.swift").find("enum BarViewTheme") != std::string::npos);
}
