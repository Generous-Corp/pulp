// SPDX-License-Identifier: MIT
#include "kit_profile_verification.hpp"
#include "json_writer.hpp"
#include "kit_manifest_validation.hpp"
#include <cctype>
#include <fstream>
#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/crypto.hpp>
#include <system_error>

namespace pulp::cli::kit {
namespace {
int int_field(const JsonValue& value, const std::string& key, int fallback = 0) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Number ? field->as_int() : fallback;
}
JsonValue parse_manifest_json(const fs::path& path) {
    const auto text = read_text(path);
    if (text.empty())
        return {};
    JsonParser parser{text};
    return parser.parse();
}
bool write_text(const fs::path& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out)
        return false;
    out << body;
    return out.good();
}
#ifdef _WIN32
std::string shell_quote_local(const fs::path& path) {
    std::string out = "'";
    for (char c : path.string())
        out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}
#endif
std::string json_string_local(const std::string& value) {
    return pulp::cli::json_string(value);
}
std::string issues_json(const std::vector<KitIssue>& issues) {
    std::string out = "[";
    for (std::size_t i = 0; i < issues.size(); ++i) {
        if (i)
            out += ",";
        out += "{\"severity\":" + json_string_local(issues[i].severity) +
               ",\"code\":" + json_string_local(issues[i].code) +
               ",\"message\":" + json_string_local(issues[i].message) + "}";
    }
    return out + "]";
}
std::string sanitize_id_for_cmake(const std::string& id) {
    std::string out = "pulp_kit_";
    for (char c : id)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out;
}
std::string copied_kit_rel_path(const std::string& kit_id, const std::string& rel) {
    return (fs::path("pulp-kits") / kit_id / fs::path(rel)).generic_string();
}
} // namespace

std::string profile_result_json(const KitProfileResult& profile) {
    std::string artifacts = "[";
    for (std::size_t i = 0; i < profile.artifacts.size(); ++i) {
        if (i)
            artifacts += ",";
        artifacts += "{\"kind\":" + json_string(profile.artifacts[i].first) +
                     ",\"path\":" + json_string(profile.artifacts[i].second) + "}";
    }
    artifacts += "]";
    return "{\"path\":" + json_string(profile.path) + ",\"kind\":" + json_string(profile.kind) +
           ",\"status\":" + json_string(profile.status) + ",\"artifacts\":" + artifacts +
           ",\"issues\":" + issues_json(profile.issues) + "}";
}

std::string profile_results_json(const std::vector<KitProfileResult>& profiles) {
    std::string out = "[";
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (i)
            out += ",";
        out += profile_result_json(profiles[i]);
    }
    out += "]";
    return out;
}

void add_profile_issue(KitProfileResult& profile, std::string severity, std::string code,
                       std::string message) {
    profile.issues.push_back({std::move(severity), std::move(code), std::move(message)});
}

bool profile_ok(const KitProfileResult& profile) {
    return std::none_of(profile.issues.begin(), profile.issues.end(),
                        [](const KitIssue& issue) { return issue.severity == "error"; });
}

bool manifest_exports_path(const JsonValue& manifest, const std::string& export_key,
                           const std::string& rel_path);

bool string_array_contains(const JsonValue& object, const std::string& key,
                           const std::string& needle) {
    const auto values = string_array_field(object, key);
    return std::find(values.begin(), values.end(), needle) != values.end();
}

bool valid_hex_string(std::string_view value, std::size_t expected_chars) {
    if (value.size() != expected_chars)
        return false;
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool graph_contains_node_type(const JsonValue& graph, const std::string& node_type) {
    auto* nodes = array_field(graph, "nodes");
    if (!nodes)
        return false;
    for (const auto& node : nodes->arr()) {
        if (node.type == JsonValue::Object && string_field(node, "type") == node_type)
            return true;
    }
    return false;
}

void verify_graph_fixture_shape(KitProfileResult& profile, const JsonValue& graph_json) {
    if (string_field(graph_json, "kind") != "signal-graph-fixture") {
        add_profile_issue(profile, "error", "invalid-graph-fixture",
                          "Graph fixture kind must be `signal-graph-fixture`");
    }
    auto* nodes = array_field(graph_json, "nodes");
    if (!nodes || nodes->arr().empty()) {
        add_profile_issue(profile, "error", "invalid-graph-fixture",
                          "Graph fixture must declare at least one node");
    } else {
        for (const auto& node : nodes->arr()) {
            if (node.type != JsonValue::Object || string_field(node, "id").empty() ||
                string_field(node, "type").empty()) {
                add_profile_issue(profile, "error", "invalid-graph-node",
                                  "Graph fixture nodes must declare non-empty id and type");
                break;
            }
        }
    }
    auto* connections = array_field(graph_json, "connections");
    if (!connections) {
        add_profile_issue(profile, "error", "invalid-graph-fixture",
                          "Graph fixture must declare a connections array");
    }
}

void verify_state_fixture_shape(KitProfileResult& profile, const JsonValue& state_json) {
    if (string_field(state_json, "kind") != "custom-node-state-fixture") {
        add_profile_issue(profile, "error", "invalid-state-fixture",
                          "State fixture kind must be `custom-node-state-fixture`");
    }
    if (string_field(state_json, "nodeType").empty()) {
        add_profile_issue(profile, "error", "invalid-state-fixture",
                          "State fixture must declare nodeType");
    }
    if (int_field(state_json, "version") <= 0) {
        add_profile_issue(profile, "error", "invalid-state-fixture",
                          "State fixture version must be positive");
    }
    if (!object_field(state_json, "state")) {
        add_profile_issue(profile, "error", "invalid-state-fixture",
                          "State fixture must declare a state object");
    }
}

KitProfileResult verify_signal_graph_state_profile(const fs::path& root, const JsonValue& manifest,
                                                   const fs::path& profile_rel,
                                                   const JsonValue& profile_json) {
    KitProfileResult profile;
    profile.path = profile_rel.generic_string();
    profile.kind = "signal-graph-state-validation";
    profile.status = "pass";

    for (const auto* check : {"register-custom-node-type", "save-load-state", "reprepare",
                              "process-no-alloc-no-lock"}) {
        if (!string_array_contains(profile_json, "checks", check)) {
            add_profile_issue(profile, "error", "missing-graph-state-check",
                              std::string("Graph/state profile must declare check `") + check +
                                  "`");
        }
    }

    const auto graph_rel = string_field(profile_json, "graphFixture");
    const auto state_rel = string_field(profile_json, "stateFixture");
    if (graph_rel.empty() || !manifest_exports_path(manifest, "graphFixtures", graph_rel)) {
        add_profile_issue(profile, "error", "graph-fixture-not-exported",
                          "graphFixture must reference an exported `graphFixtures` path");
    }
    if (state_rel.empty() || !manifest_exports_path(manifest, "stateFixtures", state_rel)) {
        add_profile_issue(profile, "error", "state-fixture-not-exported",
                          "stateFixture must reference an exported `stateFixtures` path");
    }
    if (!graph_rel.empty() && !path_value_exists(root, graph_rel)) {
        add_profile_issue(profile, "error", "missing-graph-fixture",
                          "graphFixture is missing or unsafe");
    }
    if (!state_rel.empty() && !path_value_exists(root, state_rel)) {
        add_profile_issue(profile, "error", "missing-state-fixture",
                          "stateFixture is missing or unsafe");
    }

    JsonValue graph_json;
    JsonValue state_json;
    if (!graph_rel.empty() && path_value_exists(root, graph_rel)) {
        graph_json = parse_manifest_json(root / fs::path(graph_rel));
        if (graph_json.type != JsonValue::Object) {
            add_profile_issue(profile, "error", "invalid-graph-fixture",
                              "Graph fixture must be a JSON object");
        } else {
            verify_graph_fixture_shape(profile, graph_json);
        }
    }
    if (!state_rel.empty() && path_value_exists(root, state_rel)) {
        state_json = parse_manifest_json(root / fs::path(state_rel));
        if (state_json.type != JsonValue::Object) {
            add_profile_issue(profile, "error", "invalid-state-fixture",
                              "State fixture must be a JSON object");
        } else {
            verify_state_fixture_shape(profile, state_json);
        }
    }
    const auto node_type = string_field(state_json, "nodeType");
    if (!node_type.empty() && graph_json.type == JsonValue::Object &&
        !graph_contains_node_type(graph_json, node_type)) {
        add_profile_issue(profile, "error", "graph-state-node-mismatch",
                          "State fixture nodeType must appear in graph fixture nodes");
    }

    if (!profile_ok(profile))
        profile.status = "fail";
    return profile;
}

KitProfileResult verify_node_pack_profile(const fs::path& root, const JsonValue& manifest,
                                          const fs::path& profile_rel,
                                          const JsonValue& profile_json) {
    KitProfileResult profile;
    profile.path = profile_rel.generic_string();
    profile.kind = "node-pack-validation-profile";
    profile.status = "pass";

    for (const auto* check : {"manifest-shape", "signature-before-load", "hash-before-load"}) {
        if (!string_array_contains(profile_json, "checks", check)) {
            add_profile_issue(profile, "error", "missing-node-pack-check",
                              std::string("Node-pack profile must declare check `") + check + "`");
        }
    }
    if (bool_field(profile_json, "executeDuringInspect", true)) {
        add_profile_issue(profile, "error", "node-pack-executes-during-inspect",
                          "Node-pack profiles must set executeDuringInspect to false");
    }

    const auto manifest_rel = string_field(profile_json, "manifest");
    if (manifest_rel.empty() ||
        !manifest_exports_path(manifest, "nodePackManifests", manifest_rel)) {
        add_profile_issue(
            profile, "error", "node-pack-manifest-not-exported",
            "Node-pack profile manifest must reference an exported `nodePackManifests` path");
    }
    if (!manifest_rel.empty() && !path_value_exists(root, manifest_rel)) {
        add_profile_issue(profile, "error", "missing-node-pack-manifest",
                          "Node-pack manifest is missing or unsafe");
    }
    if (!manifest_rel.empty() && path_value_exists(root, manifest_rel)) {
        const auto node_manifest = parse_manifest_json(root / fs::path(manifest_rel));
        if (node_manifest.type != JsonValue::Object) {
            add_profile_issue(profile, "error", "invalid-node-pack-manifest",
                              "Node-pack manifest must be a JSON object");
        } else {
            if (string_field(node_manifest, "pack_id").empty()) {
                add_profile_issue(profile, "error", "invalid-node-pack-manifest",
                                  "Node-pack manifest must declare pack_id");
            }
            if (int_field(node_manifest, "abi_major") <= 0) {
                add_profile_issue(profile, "error", "invalid-node-pack-manifest",
                                  "Node-pack manifest abi_major must be positive");
            }
            if (string_field(node_manifest, "binary").empty()) {
                add_profile_issue(profile, "error", "invalid-node-pack-manifest",
                                  "Node-pack manifest must declare binary");
            }
            if (!valid_hex_string(string_field(node_manifest, "sha256"), 64)) {
                add_profile_issue(profile, "error", "invalid-node-pack-hash",
                                  "Node-pack manifest sha256 must be 64 hex characters");
            }
            if (!valid_hex_string(string_field(node_manifest, "signer_public_key"), 64)) {
                add_profile_issue(profile, "error", "invalid-node-pack-public-key",
                                  "Node-pack signer_public_key must be 32 bytes encoded as hex");
            }
            if (!valid_hex_string(string_field(node_manifest, "signature"), 128)) {
                add_profile_issue(profile, "error", "invalid-node-pack-signature",
                                  "Node-pack signature must be 64 bytes encoded as hex");
            }
            auto* nodes = array_field(node_manifest, "nodes");
            if (!nodes || nodes->arr().empty()) {
                add_profile_issue(profile, "error", "invalid-node-pack-nodes",
                                  "Node-pack manifest must declare at least one node");
            } else {
                for (const auto& node : nodes->arr()) {
                    if (node.type != JsonValue::Object || string_field(node, "type_id").empty() ||
                        node.get("capabilities") == nullptr ||
                        node.get("capabilities")->type != JsonValue::Number) {
                        add_profile_issue(
                            profile, "error", "invalid-node-pack-nodes",
                            "Node-pack nodes must declare type_id and numeric capabilities");
                        break;
                    }
                }
            }
        }
    }

    if (!profile_ok(profile))
        profile.status = "fail";
    return profile;
}

bool extension_in(const fs::path& path, std::initializer_list<std::string_view> extensions) {
    const auto ext = path.extension().string();
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](std::string_view expected) { return ext == expected; });
}

KitProfileResult verify_native_component_profile(const fs::path& root, const JsonValue& manifest,
                                                 const fs::path& profile_rel,
                                                 const JsonValue& profile_json) {
    KitProfileResult profile;
    profile.path = profile_rel.generic_string();
    profile.kind = "native-component-validation-profile";
    profile.status = "pass";

    for (const auto* check : {"public-native-core-abi", "source-built-only-when-selected",
                              "process-no-alloc-no-lock"}) {
        if (!string_array_contains(profile_json, "checks", check)) {
            add_profile_issue(profile, "error", "missing-native-component-check",
                              std::string("Native-component profile must declare check `") + check +
                                  "`");
        }
    }

    auto* exports = object_field(manifest, "exports");
    const auto headers = exports ? string_array_field(*exports, "nativeComponentHeaders")
                                 : std::vector<std::string>{};
    const auto sources = exports ? string_array_field(*exports, "nativeComponentSources")
                                 : std::vector<std::string>{};
    if (headers.empty() && sources.empty()) {
        add_profile_issue(profile, "error", "missing-native-component-files",
                          "Native-component kits must export headers or sources");
    }
    for (const auto& header : headers) {
        if (!path_value_exists(root, header) ||
            !extension_in(header, {".h", ".hpp", ".hh", ".hxx"})) {
            add_profile_issue(profile, "error", "invalid-native-component-header",
                              "Native-component headers must be existing relative header paths");
        }
    }
    for (const auto& source : sources) {
        if (!path_value_exists(root, source) ||
            !extension_in(source, {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"})) {
            add_profile_issue(profile, "error", "invalid-native-component-source",
                              "Native-component sources must be existing relative source paths");
        }
    }

    auto* realtime = object_field(manifest, "realtime");
    if (!realtime || !bool_field(*realtime, "processSafe", false) ||
        bool_field(*realtime, "allocatesInProcess", true) ||
        bool_field(*realtime, "locksInProcess", true)) {
        add_profile_issue(profile, "error", "native-component-rt-contract",
                          "Native-component realtime block must declare processSafe true with no "
                          "process allocation or locks");
    }

    if (!profile_ok(profile))
        profile.status = "fail";
    return profile;
}

bool manifest_exports_path(const JsonValue& manifest, const std::string& export_key,
                           const std::string& rel_path) {
    if (auto* exports = object_field(manifest, "exports")) {
        const auto paths = string_array_field(*exports, export_key);
        return std::find(paths.begin(), paths.end(), rel_path) != paths.end();
    }
    return false;
}

bool dimensions_match(const JsonValue& profile_dims, const JsonValue& report_dims) {
    return int_field(profile_dims, "width") == int_field(report_dims, "width") &&
           int_field(profile_dims, "height") == int_field(report_dims, "height") &&
           int_field(profile_dims, "scale") == int_field(report_dims, "scale");
}

#ifdef _WIN32
std::string shell_quote_local(const fs::path& path) {
    const auto s = path.string();
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')
            out += "\\\"";
        else
            out += c;
    }
    out += "\"";
    return out;
}
#endif

std::string safe_artifact_stem(std::string value) {
    if (value.empty())
        value = "profile";
    for (char& c : value) {
        const auto ch = static_cast<unsigned char>(c);
        if (!std::isalnum(ch) && c != '-' && c != '_')
            c = '-';
    }
    return value;
}

void add_ui_kit_integration_preview(KitProfileResult& profile, const JsonValue& manifest,
                                    const std::string& kit_id, const std::string& entrypoint) {
    if (!profile_ok(profile))
        return;
    if (!string_array_contains(manifest, "kind", "ui-kit"))
        return;
    if (entrypoint.empty())
        return;

    auto* exports = object_field(manifest, "exports");
    if (!exports)
        return;

    const auto scripts = string_array_field(*exports, "pulpUiScripts");
    if (std::find(scripts.begin(), scripts.end(), entrypoint) == scripts.end())
        return;

    const auto copied_script = copied_kit_rel_path(kit_id, entrypoint);
    const auto tokens = string_array_field(*exports, "designTokens");
    const auto assets = string_array_field(*exports, "assets");
    const auto kit_target = sanitize_id_for_cmake(kit_id);

    std::string helper =
        "pulp_use_kit_ui(<plugin-target> " + kit_target + " SCRIPT " + copied_script;
    if (tokens.size() == 1) {
        helper += " TOKENS " + copied_kit_rel_path(kit_id, tokens.front());
    } else if (tokens.size() > 1) {
        helper += " TOKENS <reviewed-token-path>";
    }
    helper += ")";

    profile.artifacts.push_back(
        {"ui-kit-cmake-include", "include(cmake/pulp-kits.cmake OPTIONAL)"});
    profile.artifacts.push_back({"ui-kit-cmake-target", kit_target});
    profile.artifacts.push_back({"ui-kit-helper-call", helper});
    profile.artifacts.push_back({"ui-kit-script", copied_script});
    for (const auto& asset : assets) {
        profile.artifacts.push_back({"ui-kit-asset-root", copied_kit_rel_path(kit_id, asset)});
    }
}

fs::path default_screenshot_tool_for_project(const fs::path& project_root) {
    const auto base = project_root / "build" / "tools" / "screenshot" / "pulp-screenshot";
#ifdef _WIN32
    for (const auto& suffix : {".exe", ".cmd", ".bat"}) {
        auto candidate = base;
        candidate += suffix;
        if (fs::exists(candidate))
            return candidate;
    }
    auto exe = base;
    exe += ".exe";
    return exe;
#else
    return base;
#endif
}

std::string visual_diff_report_json(const fs::path& expected, const fs::path& actual,
                                    std::size_t expected_size, std::size_t actual_size,
                                    std::size_t differing_bytes, int tolerance_bytes, bool pass) {
    const auto mode = tolerance_bytes > 0 ? "byte-tolerance" : "exact-bytes";
    return std::string("{\n") + "  \"kind\": \"pulp-screenshot-visual-diff\",\n" +
           "  \"status\": " + json_string(pass ? "pass" : "fail") + ",\n" +
           "  \"mode\": " + json_string(mode) + ",\n" +
           "  \"expected\": " + json_string(expected.string()) + ",\n" +
           "  \"actual\": " + json_string(actual.string()) + ",\n" +
           "  \"expected_bytes\": " + std::to_string(expected_size) + ",\n" +
           "  \"actual_bytes\": " + std::to_string(actual_size) + ",\n" +
           "  \"differing_bytes\": " + std::to_string(differing_bytes) + ",\n" +
           "  \"tolerance_bytes\": " + std::to_string(tolerance_bytes) + "\n" + "}\n";
}

void maybe_write_visual_diff(KitProfileResult& profile, const fs::path& root,
                             const JsonValue& profile_json, const fs::path& output,
                             const fs::path& out_dir, const std::string& stem) {
    const auto expected_rel = string_field(profile_json, "expectedImage");
    if (expected_rel.empty())
        return;

    const int tolerance_bytes = int_field(profile_json, "visualToleranceBytes", 0);
    const auto expected_path = root / fs::path(expected_rel);
    const auto expected = read_bytes(expected_path);
    const auto actual = read_bytes(output);
    const auto common = std::min(expected.size(), actual.size());
    std::size_t differing = expected.size() > actual.size() ? expected.size() - actual.size()
                                                            : actual.size() - expected.size();
    for (std::size_t i = 0; i < common; ++i) {
        if (expected[i] != actual[i])
            ++differing;
    }

    const bool pass = differing <= static_cast<std::size_t>(std::max(0, tolerance_bytes));
    const auto diff_report = out_dir / (stem + ".visual-diff.json");
    if (write_text(diff_report,
                   visual_diff_report_json(expected_path, output, expected.size(), actual.size(),
                                           differing, tolerance_bytes, pass))) {
        profile.artifacts.push_back({"visual-diff-report", diff_report.string()});
    }
    if (!pass) {
        add_profile_issue(profile, "error", "screenshot-visual-diff-mismatch",
                          "Rendered screenshot does not match expectedImage baseline");
        profile.status = "fail";
    }
}

void maybe_execute_screenshot_profile(KitProfileResult& profile, const fs::path& root,
                                      const fs::path& project_root, const JsonValue& profile_json,
                                      const JsonValue& dimensions,
                                      const KitVerifyOptions& options) {
    if (!options.execute_screenshots)
        return;
    if (!profile_ok(profile))
        return;

    const auto entrypoint = string_field(profile_json, "entrypoint");
    const auto screenshot_tool = default_screenshot_tool_for_project(project_root);
    if (!fs::exists(screenshot_tool)) {
        add_profile_issue(profile, "error", "missing-screenshot-tool",
                          "Cannot execute screenshot profile because "
                          "build/tools/screenshot/pulp-screenshot is missing");
        profile.status = "fail";
        return;
    }

    const auto out_dir =
        options.screenshot_output_dir.empty()
            ? project_root / ".pulp" / "kit-validation" / safe_artifact_stem(profile.path)
            : options.screenshot_output_dir;
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        add_profile_issue(profile, "error", "screenshot-output-dir",
                          "Failed to create screenshot output directory: " + ec.message());
        profile.status = "fail";
        return;
    }

    const auto stem = safe_artifact_stem(string_field(profile_json, "id").empty()
                                             ? fs::path(profile.path).stem().string()
                                             : string_field(profile_json, "id"));
    const auto output = out_dir / (stem + ".png");
    const auto log = out_dir / (stem + ".log");
    const int width = int_field(dimensions, "width");
    const int height = int_field(dimensions, "height");
    const int scale = int_field(dimensions, "scale");

    const std::vector<std::string> screenshot_args{
        "--script",  (root / fs::path(entrypoint)).string(),
        "--output",  output.string(),
        "--width",   std::to_string(width),
        "--height",  std::to_string(height),
        "--scale",   std::to_string(scale),
        "--backend", options.screenshot_backend,
    };
    pulp::platform::ProcessResult result;
#ifdef _WIN32
    const auto screenshot_ext = screenshot_tool.extension().string();
    if (screenshot_ext == ".cmd" || screenshot_ext == ".bat") {
        std::string shell_command = "call " + shell_quote_local(screenshot_tool);
        for (const auto& arg : screenshot_args) {
            shell_command += " " + shell_quote_local(fs::path(arg));
        }
        result = pulp::platform::exec("cmd", {"/C", shell_command}, 120000);
    } else {
        result = pulp::platform::exec(screenshot_tool.string(), screenshot_args, 120000);
    }
#else
    result = pulp::platform::exec(screenshot_tool.string(), screenshot_args, 120000);
#endif
    (void)write_text(log, result.stdout_output + result.stderr_output);
    profile.artifacts.push_back({"render-log", log.string()});
    if (result.exit_code != 0 || !fs::exists(output) || fs::file_size(output, ec) == 0 || ec) {
        add_profile_issue(profile, "error", "screenshot-render-failed",
                          "Screenshot execution failed; see render log");
        profile.status = "fail";
        return;
    }
    profile.artifacts.push_back({"rendered-screenshot", output.string()});
    maybe_write_visual_diff(profile, root, profile_json, output, out_dir, stem);
}

KitProfileResult verify_screenshot_profile(const fs::path& root, const JsonValue& manifest,
                                           const fs::path& profile_rel,
                                           const JsonValue& profile_json,
                                           const fs::path& project_root,
                                           const KitVerifyOptions& options) {
    KitProfileResult profile;
    profile.path = profile_rel.generic_string();
    profile.kind = "pulp-screenshot-profile";
    profile.status = "pass";

    const auto entrypoint = string_field(profile_json, "entrypoint");
    if (entrypoint.empty() || !manifest_exports_path(manifest, "pulpUiScripts", entrypoint)) {
        add_profile_issue(
            profile, "error", "screenshot-entrypoint-not-exported",
            "Screenshot profile entrypoint must reference an exported `pulpUiScripts` path");
    }

    auto* dimensions = object_field(profile_json, "dimensions");
    if (!dimensions || int_field(*dimensions, "width") <= 0 ||
        int_field(*dimensions, "height") <= 0 || int_field(*dimensions, "scale") <= 0) {
        add_profile_issue(
            profile, "error", "invalid-screenshot-dimensions",
            "Screenshot profile dimensions must include positive width, height, and scale");
    }

    if (auto* policy = object_field(profile_json, "policy")) {
        if (string_field(*policy, "renderer") != "pulp") {
            add_profile_issue(profile, "error", "unsupported-screenshot-renderer",
                              "Screenshot profiles must use renderer `pulp`");
        }
        if (bool_field(*policy, "executeDuringInspect", false)) {
            add_profile_issue(profile, "error", "screenshot-executes-during-inspect",
                              "Screenshot profiles must not execute during inspect/plan");
        }
    } else {
        add_profile_issue(profile, "error", "missing-screenshot-policy",
                          "Screenshot profiles must declare a policy block");
    }

    const auto report_rel = string_field(profile_json, "expectedReport");
    if (report_rel.empty()) {
        add_profile_issue(profile, "error", "missing-screenshot-report",
                          "Screenshot profile must declare `expectedReport`");
    } else if (!path_value_exists(root, report_rel)) {
        add_profile_issue(profile, "error", "missing-screenshot-report",
                          "Screenshot profile expectedReport is missing or unsafe");
    } else {
        const auto report_path = root / fs::path(report_rel);
        auto report_json = parse_manifest_json(report_path);
        if (report_json.type != JsonValue::Object) {
            add_profile_issue(profile, "error", "invalid-screenshot-report",
                              "Screenshot report must be a JSON object");
        } else {
            if (string_field(report_json, "kind") != "pulp-screenshot-report") {
                add_profile_issue(profile, "error", "invalid-screenshot-report",
                                  "Screenshot report kind must be `pulp-screenshot-report`");
            }
            if (string_field(report_json, "profile") != profile.path) {
                add_profile_issue(
                    profile, "error", "screenshot-report-profile-mismatch",
                    "Screenshot report profile path must match the validation profile");
            }
            if (string_field(report_json, "renderer") != "pulp") {
                add_profile_issue(profile, "error", "screenshot-report-renderer-mismatch",
                                  "Screenshot report renderer must be `pulp`");
            }
            auto* report_dims = object_field(report_json, "dimensions");
            if (dimensions && (!report_dims || !dimensions_match(*dimensions, *report_dims))) {
                add_profile_issue(profile, "error", "screenshot-report-dimensions-mismatch",
                                  "Screenshot report dimensions must match the validation profile");
            }
        }
    }

    const auto expected_image_rel = string_field(profile_json, "expectedImage");
    if (!expected_image_rel.empty() && !path_value_exists(root, expected_image_rel)) {
        add_profile_issue(profile, "error", "missing-screenshot-baseline",
                          "Screenshot profile expectedImage is missing or unsafe");
    }
    if (has_field(profile_json, "visualToleranceBytes") &&
        int_field(profile_json, "visualToleranceBytes", 0) < 0) {
        add_profile_issue(profile, "error", "invalid-screenshot-visual-tolerance",
                          "Screenshot profile visualToleranceBytes must be zero or greater");
    }

    if (dimensions && project_root.empty() && options.execute_screenshots) {
        add_profile_issue(
            profile, "error", "missing-project-root",
            "Screenshot execution requires --project or a Pulp project working directory");
    } else if (dimensions) {
        maybe_execute_screenshot_profile(profile, root, project_root, profile_json, *dimensions,
                                         options);
    }

    add_ui_kit_integration_preview(profile, manifest, string_field(manifest, "id"), entrypoint);

    if (!profile_ok(profile))
        profile.status = "fail";
    return profile;
}

KitProfileResult verify_generic_profile(const fs::path& profile_rel,
                                        const JsonValue& profile_json) {
    KitProfileResult profile;
    profile.path = profile_rel.generic_string();
    profile.kind = string_field(profile_json, "kind");
    if (profile.kind.empty()) {
        profile.kind = "unknown";
        profile.status = "fail";
        add_profile_issue(profile, "error", "missing-profile-kind",
                          "Validation profile must declare `kind`");
    } else {
        profile.status = "skipped";
        add_profile_issue(
            profile, "info", "profile-requires-validation-lane",
            "Profile exists but execution belongs to its build/runtime validation lane");
    }
    return profile;
}

std::vector<KitProfileResult> verify_profiles(const fs::path& manifest_path,
                                              const JsonValue& manifest,
                                              const fs::path& project_root,
                                              const KitVerifyOptions& options) {
    std::vector<KitProfileResult> profiles;
    const auto root = manifest_root_for(manifest_path);
    auto* validation = object_field(manifest, "validation");
    if (!validation)
        return profiles;
    for (const auto& rel : string_array_field(*validation, "profiles")) {
        if (!path_value_exists(root, rel)) {
            KitProfileResult profile;
            profile.path = rel;
            profile.status = "fail";
            add_profile_issue(profile, "error", "missing-profile",
                              "Validation profile is missing or unsafe");
            profiles.push_back(std::move(profile));
            continue;
        }
        auto profile_rel = fs::path(rel);
        auto profile_json = parse_manifest_json(root / profile_rel);
        if (profile_json.type != JsonValue::Object) {
            KitProfileResult profile;
            profile.path = profile_rel.generic_string();
            profile.status = "fail";
            add_profile_issue(profile, "error", "invalid-profile-json",
                              "Validation profile must be a JSON object");
            profiles.push_back(std::move(profile));
            continue;
        }
        const auto kind = string_field(profile_json, "kind");
        if (kind == "pulp-screenshot-profile") {
            profiles.push_back(verify_screenshot_profile(root, manifest, profile_rel, profile_json,
                                                         project_root, options));
        } else if (kind == "signal-graph-state-validation") {
            profiles.push_back(
                verify_signal_graph_state_profile(root, manifest, profile_rel, profile_json));
        } else if (kind == "node-pack-validation-profile") {
            profiles.push_back(verify_node_pack_profile(root, manifest, profile_rel, profile_json));
        } else if (kind == "native-component-validation-profile") {
            profiles.push_back(
                verify_native_component_profile(root, manifest, profile_rel, profile_json));
        } else {
            profiles.push_back(verify_generic_profile(profile_rel, profile_json));
        }
    }
    return profiles;
}

} // namespace pulp::cli::kit
