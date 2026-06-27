// Tests for the validation report contract schema.
// Verifies the JSON schema is well-formed and that example payloads
// match the expected structure documented in validation-report-v1.schema.json.

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>

// Read a file into a string
static std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path);
    REQUIRE(f.good());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Minimal JSON structure check: verifies required keys exist in a JSON string.
// Not a full JSON parser — just validates that key structural markers are present.
static bool json_contains_key(const std::string& json, const std::string& key) {
    auto quoted = "\"" + key + "\"";
    return json.find(quoted) != std::string::npos;
}

// Find the project root by walking up from the test binary directory
static std::filesystem::path find_project_root() {
    // Prefer the compile-time source file location so detached validation
    // worktrees under /tmp still resolve the source tree correctly.
    auto source_file = std::filesystem::path(__FILE__);
    if (!source_file.empty()) {
        auto candidate = source_file.parent_path().parent_path();
        auto schema = candidate / "docs" / "contracts" / "validation-report-v1.schema.json";
        if (std::filesystem::exists(schema)) return std::filesystem::canonical(candidate);
    }

    // Try common relative paths from where ctest runs
    for (auto candidate : {
        std::filesystem::current_path(),
        std::filesystem::current_path() / "..",
        std::filesystem::current_path() / "../..",
        std::filesystem::current_path() / "../../..",
    }) {
        auto schema = candidate / "docs" / "contracts" / "validation-report-v1.schema.json";
        if (std::filesystem::exists(schema)) return std::filesystem::canonical(candidate);
    }
    return {};
}

TEST_CASE("Validation schema file exists and is well-formed JSON", "[contract][validation-report]") {
    auto root = find_project_root();
    REQUIRE_FALSE(root.empty());

    auto schema_path = root / "docs" / "contracts" / "validation-report-v1.schema.json";
    REQUIRE(std::filesystem::exists(schema_path));

    auto content = read_file(schema_path);
    REQUIRE_FALSE(content.empty());

    // Verify it's a JSON Schema document
    REQUIRE(json_contains_key(content, "$schema"));
    REQUIRE(json_contains_key(content, "$id"));
    REQUIRE(json_contains_key(content, "title"));
    REQUIRE(json_contains_key(content, "type"));
    REQUIRE(json_contains_key(content, "required"));
    REQUIRE(json_contains_key(content, "properties"));
    REQUIRE(json_contains_key(content, "$defs"));
}

TEST_CASE("Schema defines all required report types", "[contract][validation-report]") {
    auto root = find_project_root();
    REQUIRE_FALSE(root.empty());

    auto content = read_file(root / "docs" / "contracts" / "validation-report-v1.schema.json");

    // All six report types from the contract
    REQUIRE(json_contains_key(content, "screenshot"));
    REQUIRE(json_contains_key(content, "screenshot_diff"));
    REQUIRE(json_contains_key(content, "inspector"));
    REQUIRE(json_contains_key(content, "validator"));
    REQUIRE(json_contains_key(content, "sanitizer"));
    REQUIRE(json_contains_key(content, "test_suite"));
}

TEST_CASE("Schema defines screenshot metadata fields", "[contract][validation-report]") {
    auto root = find_project_root();
    REQUIRE_FALSE(root.empty());

    auto content = read_file(root / "docs" / "contracts" / "validation-report-v1.schema.json");

    REQUIRE(json_contains_key(content, "screenshot_metadata"));
    REQUIRE(json_contains_key(content, "width"));
    REQUIRE(json_contains_key(content, "height"));
    REQUIRE(json_contains_key(content, "scale"));
    REQUIRE(json_contains_key(content, "backend"));
}

TEST_CASE("Schema defines diff metrics fields", "[contract][validation-report]") {
    auto root = find_project_root();
    REQUIRE_FALSE(root.empty());

    auto content = read_file(root / "docs" / "contracts" / "validation-report-v1.schema.json");

    REQUIRE(json_contains_key(content, "diff_metrics"));
    REQUIRE(json_contains_key(content, "similarity"));
    REQUIRE(json_contains_key(content, "total_pixels"));
    REQUIRE(json_contains_key(content, "diff_pixels"));
    REQUIRE(json_contains_key(content, "mean_error"));
    REQUIRE(json_contains_key(content, "tolerance"));
    REQUIRE(json_contains_key(content, "diff_bounds"));
}

TEST_CASE("Schema defines validator result fields", "[contract][validation-report]") {
    auto root = find_project_root();
    REQUIRE_FALSE(root.empty());

    auto content = read_file(root / "docs" / "contracts" / "validation-report-v1.schema.json");

    REQUIRE(json_contains_key(content, "validator_result"));
    REQUIRE(json_contains_key(content, "pluginval"));
    REQUIRE(json_contains_key(content, "clap-validator"));
    REQUIRE(json_contains_key(content, "auval"));
    REQUIRE(json_contains_key(content, "vstvalidator"));
    REQUIRE(json_contains_key(content, "plugin_path"));
    REQUIRE(json_contains_key(content, "exit_code"));
}

TEST_CASE("Schema defines sanitizer metadata fields", "[contract][validation-report]") {
    auto root = find_project_root();
    REQUIRE_FALSE(root.empty());

    auto content = read_file(root / "docs" / "contracts" / "validation-report-v1.schema.json");

    REQUIRE(json_contains_key(content, "sanitizer_metadata"));
    REQUIRE(json_contains_key(content, "asan"));
    REQUIRE(json_contains_key(content, "tsan"));
    REQUIRE(json_contains_key(content, "ubsan"));
    // RTSan is part of the available sanitizer set.
    REQUIRE(content.find("\"rtsan\"") != std::string::npos);
}

TEST_CASE("Schema defines inspector payload fields", "[contract][validation-report]") {
    auto root = find_project_root();
    REQUIRE_FALSE(root.empty());

    auto content = read_file(root / "docs" / "contracts" / "validation-report-v1.schema.json");

    REQUIRE(json_contains_key(content, "inspector_payload"));
    REQUIRE(json_contains_key(content, "view_tree"));
    REQUIRE(json_contains_key(content, "view_count"));
}

TEST_CASE("Reality snapshot YAML exists and covers all subsystems", "[contract][validation-report]") {
    auto root = find_project_root();
    REQUIRE_FALSE(root.empty());

    auto snapshot_path = root / "docs" / "contracts" / "phase1-reality-snapshot.yaml";
    REQUIRE(std::filesystem::exists(snapshot_path));

    auto content = read_file(snapshot_path);
    REQUIRE_FALSE(content.empty());

    // All subsystems documented in the contract
    REQUIRE(content.find("js_engine:") != std::string::npos);
    REQUIRE(content.find("multi_window:") != std::string::npos);
    REQUIRE(content.find("webview:") != std::string::npos);
    REQUIRE(content.find("audio_visualization:") != std::string::npos);
    REQUIRE(content.find("theme_system:") != std::string::npos);
    REQUIRE(content.find("widget_library:") != std::string::npos);
    REQUIRE(content.find("resource_asset_system:") != std::string::npos);
    REQUIRE(content.find("sanitizers:") != std::string::npos);
    REQUIRE(content.find("validators:") != std::string::npos);
    REQUIRE(content.find("dsp_dsls:") != std::string::npos);
    REQUIRE(content.find("webgpu_compute_audio:") != std::string::npos);
    REQUIRE(content.find("offline_video:") != std::string::npos);

    // Dependency map
    REQUIRE(content.find("phases:") != std::string::npos);
    REQUIRE(content.find("depends_on:") != std::string::npos);
}

TEST_CASE("Reality snapshot does not overclaim", "[contract][validation-report]") {
    auto root = find_project_root();
    REQUIRE_FALSE(root.empty());

    auto content = read_file(root / "docs" / "contracts" / "phase1-reality-snapshot.yaml");

    // Acceptance criteria: these must NOT appear as "shipped" or "real today"
    // JSC/V8 backend
    REQUIRE(content.find("backend: jsc") == std::string::npos);
    REQUIRE(content.find("backend: v8") == std::string::npos);

    // RTSan should appear in the available list.
    auto rtsan_pos = content.find("rtsan");
    REQUIRE(rtsan_pos != std::string::npos);
    // It should be in the "available:" list, not "not_integrated:"
    REQUIRE(content.find("not_integrated:") == std::string::npos);

    // DSP DSLs must be planned, not shipped
    auto dsl_pos = content.find("dsp_dsls:");
    REQUIRE(dsl_pos != std::string::npos);
    auto dsl_section = content.substr(dsl_pos, 200);
    REQUIRE(dsl_section.find("status: planned") != std::string::npos);

    // Offline video must be planned
    auto video_pos = content.find("offline_video:");
    REQUIRE(video_pos != std::string::npos);
    auto video_section = content.substr(video_pos, 200);
    REQUIRE(video_section.find("status: planned") != std::string::npos);

    // WebGPU compute must be planned
    auto wgpu_pos = content.find("webgpu_compute_audio:");
    REQUIRE(wgpu_pos != std::string::npos);
    auto wgpu_section = content.substr(wgpu_pos, 200);
    REQUIRE(wgpu_section.find("status: planned") != std::string::npos);
}
