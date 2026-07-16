// SPDX-License-Identifier: MIT
//
// kit_commands.cpp — metadata-only Pulp package/kit commands.
// This file intentionally performs no package execution: no CMake, JS,
// scripts, dylibs, or tool hooks are invoked while validating or inspecting.

#include "kit_commands.hpp"

#include "json_parser.hpp"
#include "json_writer.hpp"
#include "package_registry.hpp"
#include "pulp_version_gen.h"

#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/crypto.hpp>

#include "../../external/miniz/miniz.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulp::cli::kit {

namespace {

using pulp::cli::pkg::JsonValue;
using pulp::cli::pkg::JsonParser;

std::string read_text(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string file_sha256(const fs::path& path) {
    const auto bytes = read_bytes(path);
    return pulp::runtime::sha256_hex(bytes.data(), bytes.size());
}

bool write_text(const fs::path& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << body;
    return f.good();
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::vector<std::uint8_t> hex_decode_local(std::string_view hex) {
    if (hex.size() % 2 != 0) return {};
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_value(hex[i]);
        const int lo = hex_value(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

void print_ok_local(const std::string& msg) {
    std::cout << "OK: " << msg << "\n";
}

void print_fail_local(const std::string& msg) {
    std::cerr << "Error: " << msg << "\n";
}

bool path_is_within_local(const fs::path& path, const fs::path& root) {
    auto p = path.lexically_normal();
    auto r = root.lexically_normal();
    auto pit = p.begin();
    auto rit = r.begin();
    for (; rit != r.end(); ++rit, ++pit) {
        if (pit == p.end() || *pit != *rit) return false;
    }
    return true;
}

void add_issue(KitValidationResult& result,
               std::string severity,
               std::string code,
               std::string message) {
    result.issues.push_back({std::move(severity), std::move(code), std::move(message)});
}

const JsonValue* object_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Object ? field : nullptr;
}

const JsonValue* array_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Array ? field : nullptr;
}

std::string string_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::String ? field->str_val : std::string{};
}

bool bool_field(const JsonValue& value, const std::string& key, bool fallback = false) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Bool ? field->bool_val : fallback;
}

std::vector<std::string> string_array_field(const JsonValue& value, const std::string& key) {
    if (auto* field = array_field(value, key)) return field->as_string_array();
    return {};
}

bool has_field(const JsonValue& value, const std::string& key) {
    return value.get(key) != nullptr;
}

int int_field(const JsonValue& value, const std::string& key, int fallback = 0) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Number ? field->as_int() : fallback;
}

std::vector<std::string> object_string_array_field(const JsonValue& value,
                                                   const std::string& object_key,
                                                   const std::string& array_key) {
    if (auto* object = object_field(value, object_key))
        return string_array_field(*object, array_key);
    return {};
}

fs::path resolve_manifest_path(const fs::path& input) {
    if (input.empty()) return {};
    std::error_code ec;
    if (fs::is_directory(input, ec)) return input / "pulp.package.json";
    return input;
}

fs::path manifest_root_for(const fs::path& manifest_path) {
    auto parent = manifest_path.parent_path();
    return parent.empty() ? fs::current_path() : parent;
}

bool vector_contains(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

bool valid_package_id(const std::string& id) {
    if (id.empty()) return false;
    for (char c : id) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '.' || c == '-' || c == '_' || c == ':'))
            return false;
    }
    return id.find("..") == std::string::npos;
}

bool valid_package_path_component(const std::string& id) {
    return valid_package_id(id)
        && id != "."
        && id != ".."
        && id.find(':') == std::string::npos
        && id.back() != '.';
}

bool valid_semverish(const std::string& version) {
    if (version.empty() || !std::isdigit(static_cast<unsigned char>(version.front())))
        return false;
    return std::all_of(version.begin(), version.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '+';
    });
}

bool valid_cmake_target_name(std::string_view name) {
    if (name.empty()) return false;
    if (name.find(":::") != std::string_view::npos) return false;
    if (name.starts_with(':') || name.ends_with(':')) return false;

    bool previous_was_colon = false;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-' || c == '.' || c == '+') {
            previous_was_colon = false;
            continue;
        }
        if (c == ':') {
            if (previous_was_colon) {
                previous_was_colon = false;
                continue;
            }
            previous_was_colon = true;
            continue;
        }
        return false;
    }

    return !previous_was_colon;
}

struct SemverTriple {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

bool parse_semver_triple(std::string_view text, SemverTriple& out) {
    std::size_t pos = 0;
    int parts[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (pos >= text.size()
            || !std::isdigit(static_cast<unsigned char>(text[pos]))) {
            return false;
        }
        int value = 0;
        while (pos < text.size()
               && std::isdigit(static_cast<unsigned char>(text[pos]))) {
            value = value * 10 + (text[pos] - '0');
            ++pos;
        }
        parts[i] = value;
        if (i < 2) {
            if (pos >= text.size() || text[pos] != '.') return false;
            ++pos;
        }
    }
    if (pos < text.size()
        && text[pos] != '-' && text[pos] != '+') {
        return false;
    }
    out.major = parts[0];
    out.minor = parts[1];
    out.patch = parts[2];
    return true;
}

int compare_semver(const SemverTriple& a, const SemverTriple& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::vector<std::string> split_version_requirements(const std::string& text) {
    std::vector<std::string> tokens;
    std::string token;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }
    if (!token.empty()) tokens.push_back(token);
    return tokens;
}

bool semver_satisfies_token(const SemverTriple& sdk,
                            const std::string& token,
                            std::string& error) {
    std::string op;
    std::string version = token;
    for (const auto* candidate : {">=", "<=", "==", ">", "<", "="}) {
        const std::string prefix(candidate);
        if (token.rfind(prefix, 0) == 0) {
            op = prefix;
            version = token.substr(prefix.size());
            break;
        }
    }
    if (op.empty()) op = "==";
    SemverTriple required;
    if (!parse_semver_triple(version, required)) {
        error = "Unsupported `requires.pulp` constraint `" + token
              + "`; expected comparators like >=0.395.0";
        return false;
    }
    const int cmp = compare_semver(sdk, required);
    if (op == "==" || op == "=") return cmp == 0;
    if (op == ">=") return cmp >= 0;
    if (op == "<=") return cmp <= 0;
    if (op == ">") return cmp > 0;
    if (op == "<") return cmp < 0;
    error = "Unsupported `requires.pulp` comparator `" + op + "`";
    return false;
}

void validate_pulp_sdk_requirement(KitValidationResult& result, const JsonValue& root) {
    auto* requirements = object_field(root, "requires");
    if (!requirements) return;
    auto* pulp = requirements->get("pulp");
    if (!pulp) return;
    if (pulp->type != JsonValue::String || pulp->str_val.empty()) {
        add_issue(result, "error", "invalid-sdk-requirement",
                  "`requires.pulp` must be a non-empty version constraint string");
        return;
    }
    SemverTriple sdk;
    if (!parse_semver_triple(PULP_SDK_VERSION_GENERATED, sdk)) {
        add_issue(result, "error", "invalid-sdk-version",
                  "Running Pulp SDK version is not semver-compatible: "
                      + std::string(PULP_SDK_VERSION_GENERATED));
        return;
    }
    const auto tokens = split_version_requirements(pulp->str_val);
    if (tokens.empty()) {
        add_issue(result, "error", "invalid-sdk-requirement",
                  "`requires.pulp` must contain at least one version constraint");
        return;
    }
    for (const auto& token : tokens) {
        std::string error;
        if (!semver_satisfies_token(sdk, token, error)) {
            add_issue(result, "error",
                      error.empty() ? "sdk-incompatible" : "invalid-sdk-requirement",
                      error.empty()
                          ? "Kit requires Pulp `" + pulp->str_val + "` but running SDK is "
                                + std::string(PULP_SDK_VERSION_GENERATED)
                          : error);
        }
    }
}

void validate_cpp_requirement(KitValidationResult& result, const JsonValue& root) {
    auto* requirements = object_field(root, "requires");
    if (!requirements) return;
    auto* cpp = requirements->get("cpp");
    if (!cpp) return;
    if (cpp->type != JsonValue::Number) {
        add_issue(result, "error", "invalid-cpp-requirement",
                  "`requires.cpp` must be an integer language standard such as 20");
        return;
    }
    if (std::trunc(cpp->num_val) != cpp->num_val
        || cpp->num_val > static_cast<double>(std::numeric_limits<int>::max())) {
        add_issue(result, "error", "invalid-cpp-requirement",
                  "`requires.cpp` must be an integer language standard such as 20");
        return;
    }
    const int required = cpp->as_int();
    if (required <= 0) {
        add_issue(result, "error", "invalid-cpp-requirement",
                  "`requires.cpp` must be a positive C++ language standard number");
        return;
    }
    if (required > 20) {
        add_issue(result, "error", "cpp-incompatible",
                  "Kit requires C++" + std::to_string(required)
                      + " but this Pulp SDK currently supports C++20 kit builds");
    }
}

bool valid_pulp_module_dependency(const std::string& module) {
    static const std::set<std::string> kModules = {
        "pulp::audio",
        "pulp::canvas",
        "pulp::dsl",
        "pulp::events",
        "pulp::format",
        "pulp::host",
        "pulp::inspect",
        "pulp::midi",
        "pulp::native-components",
        "pulp::platform",
        "pulp::render",
        "pulp::runtime",
        "pulp::ship",
        "pulp::signal",
        "pulp::state",
        "pulp::tool-audio",
        "pulp::view",
        "pulp::view-core",
        "pulp::view-script",
    };
    return kModules.count(module) != 0;
}

void validate_pulp_module_dependencies(KitValidationResult& result, const JsonValue& root) {
    auto* dependencies = object_field(root, "dependencies");
    if (!dependencies) return;
    auto* modules = dependencies->get("pulp");
    if (!modules) return;
    if (modules->type != JsonValue::Array) {
        add_issue(result, "error", "invalid-pulp-module-dependency",
                  "`dependencies.pulp` must be an array of known Pulp CMake targets");
        return;
    }
    for (const auto& module : modules->arr()) {
        if (module.type != JsonValue::String || module.str_val.empty()) {
            add_issue(result, "error", "invalid-pulp-module-dependency",
                      "`dependencies.pulp` entries must be non-empty strings");
            continue;
        }
        if (!valid_pulp_module_dependency(module.str_val)) {
            add_issue(result, "error", "unknown-pulp-module-dependency",
                      "Unknown Pulp module dependency `" + module.str_val + "`");
        }
    }
}

bool valid_platform(const std::string& platform) {
    static const std::set<std::string> kPlatforms = {
        "macOS", "Windows", "Linux", "iOS", "Android", "WASM", "AUv3"
    };
    return kPlatforms.count(platform) != 0;
}

void validate_string_array_items(KitValidationResult& result,
                                 const JsonValue& object,
                                 const std::string& key,
                                 const std::string& code,
                                 const std::string& label) {
    auto* field = object.get(key);
    if (!field || field->type != JsonValue::Array) return;
    for (const auto& item : field->arr()) {
        if (item.type != JsonValue::String || item.str_val.empty()) {
            add_issue(result, "error", code,
                      "`" + label + "` entries must be non-empty strings");
        }
    }
}

void validate_manifest_array_shape(KitValidationResult& result,
                                   const JsonValue& root) {
    validate_string_array_items(result, root, "kind",
                                "invalid-kind", "kind");
    validate_string_array_items(result, root, "capabilities",
                                "invalid-capability", "capabilities");
    validate_string_array_items(result, root, "audience",
                                "invalid-audience", "audience");

    static const std::set<std::string> kKinds = {
        "source", "ui-kit", "template", "content-pack", "node-pack", "native-component"
    };
    static const std::set<std::string> kAudiences = {
        "developer", "agent", "end-user"
    };
    for (const auto& kind : string_array_field(root, "kind")) {
        if (kKinds.count(kind) == 0) {
            add_issue(result, "error", "invalid-kind",
                      "`kind` contains unsupported package kind `" + kind + "`");
        }
    }
    for (const auto& audience : string_array_field(root, "audience")) {
        if (kAudiences.count(audience) == 0) {
            add_issue(result, "error", "invalid-audience",
                      "`audience` contains unsupported value `" + audience + "`");
        }
    }

    if (auto* requirements = object_field(root, "requires")) {
        validate_string_array_items(result, *requirements, "platforms",
                                    "invalid-platform", "requires.platforms");
    }
    if (auto* dependencies = object_field(root, "dependencies")) {
        validate_string_array_items(result, *dependencies, "packages",
                                    "invalid-dependency-package",
                                    "dependencies.packages");
    }
}

bool path_value_exists(const fs::path& root, const std::string& rel) {
    if (rel.empty()) return false;
    fs::path p(rel);
    if (p.is_absolute()) return false;
    std::error_code ec;
    auto normalized = fs::weakly_canonical(root / p, ec);
    if (ec) normalized = (root / p).lexically_normal();
    auto root_norm = fs::weakly_canonical(root, ec);
    if (ec) root_norm = root.lexically_normal();
    if (!path_is_within_local(normalized, root_norm)) return false;
    return fs::exists(normalized);
}

fs::path find_project_root_local() {
    auto dir = fs::current_path();
    while (!dir.empty()) {
        if (fs::exists(dir / "CMakeLists.txt") || fs::exists(dir / "pulp.toml"))
            return dir;
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

JsonValue parse_manifest_json(const fs::path& manifest_path) {
    const auto text = read_text(manifest_path);
    if (text.empty()) return {};
    JsonParser parser{text};
    return parser.parse();
}

void validate_required_string(KitValidationResult& result,
                              const JsonValue& root,
                              const std::string& key) {
    auto* field = root.get(key);
    if (!field) {
        add_issue(result, "error", "missing-field", "Missing required field `" + key + "`");
    } else if (field->type != JsonValue::String || field->str_val.empty()) {
        add_issue(result, "error", "invalid-field", "`" + key + "` must be a non-empty string");
    }
}

void validate_required_array(KitValidationResult& result,
                             const JsonValue& root,
                             const std::string& key) {
    auto* field = root.get(key);
    if (!field) {
        add_issue(result, "error", "missing-field", "Missing required field `" + key + "`");
    } else if (field->type != JsonValue::Array) {
        add_issue(result, "error", "invalid-field", "`" + key + "` must be an array");
    }
}

void validate_required_object(KitValidationResult& result,
                              const JsonValue& root,
                              const std::string& key) {
    auto* field = root.get(key);
    if (!field) {
        add_issue(result, "error", "missing-field", "Missing required field `" + key + "`");
    } else if (field->type != JsonValue::Object) {
        add_issue(result, "error", "invalid-field", "`" + key + "` must be an object");
    }
}

void validate_license(KitValidationResult& result,
                      const std::string& scope,
                      const std::string& license,
                      bool strict) {
    if (license.empty()) return;
    auto verdict = pulp::cli::pkg::check_license(license);
    if (verdict == pulp::cli::pkg::LicenseVerdict::rejected) {
        add_issue(result, "error", "license-rejected",
                  scope + " license `" + license
                      + "` is not allowed for redistributable package metadata");
    } else if (strict && verdict == pulp::cli::pkg::LicenseVerdict::review_required) {
        add_issue(result, "error", "license-review-required",
                  scope + " license `" + license + "` requires manual review");
    } else if (verdict == pulp::cli::pkg::LicenseVerdict::review_required) {
        add_issue(result, "warning", "license-review-required",
                  scope + " license `" + license + "` requires manual review");
    }
}

void validate_license_inventory(KitValidationResult& result,
                                const JsonValue& root,
                                bool strict) {
    validate_license(result, "package", string_field(root, "license"), strict);
    auto* licenses = object_field(root, "licenses");
    if (!licenses) {
        add_issue(result, "error", "missing-license-inventory",
                  "`licenses` inventory is required and must map asset classes to SPDX ids");
        return;
    }
    if (licenses->obj().empty()) {
        add_issue(result, "error", "missing-license-inventory",
                  "`licenses` inventory must include at least one asset class");
        return;
    }
    for (const auto& [scope, value] : licenses->obj()) {
        if (value.type != JsonValue::String || value.str_val.empty()) {
            add_issue(result, "error", "invalid-license",
                      "`licenses." + scope + "` must be a non-empty SPDX string");
            continue;
        }
        validate_license(result, "licenses." + scope, value.str_val, strict);
    }
}

bool non_empty_array_member(const JsonValue& object, const std::string& key) {
    auto* field = array_field(object, key);
    return field && !field->arr().empty();
}

void validate_realtime(KitValidationResult& result,
                       const JsonValue& root,
                       const std::vector<std::string>& kinds) {
    auto* rt = object_field(root, "realtime");
    auto* exports = object_field(root, "exports");
    const bool graph_source =
        exports && (non_empty_array_member(*exports, "graphFixtures")
                   || non_empty_array_member(*exports, "stateFixtures"));
    const bool requires_rt_contract =
        vector_contains(kinds, "node-pack")
        || vector_contains(kinds, "native-component")
        || graph_source;

    if (!rt) {
        if (requires_rt_contract) {
            add_issue(result, "error", "missing-rt-contract",
                      "Phase 4 native/graph kits must declare a `realtime` contract");
        }
        return;
    }
    if (requires_rt_contract) {
        for (const auto* key : {"processSafe", "allocatesInProcess", "locksInProcess"}) {
            auto* field = rt->get(key);
            if (!field || field->type != JsonValue::Bool) {
                add_issue(result, "error", "invalid-rt-contract",
                          std::string("`realtime.") + key + "` must be an explicit boolean");
            }
        }
    }
    const bool process_safe = bool_field(*rt, "processSafe", false);
    if (process_safe && bool_field(*rt, "allocatesInProcess", false)) {
        add_issue(result, "error", "rt-claim-conflict",
                  "`realtime.processSafe` cannot be true while `allocatesInProcess` is true");
    }
    if (process_safe && bool_field(*rt, "locksInProcess", false)) {
        add_issue(result, "error", "rt-claim-conflict",
                  "`realtime.processSafe` cannot be true while `locksInProcess` is true");
    }
}

void validate_platforms(KitValidationResult& result,
                        const JsonValue& root,
                        const std::vector<std::string>& kinds) {
    auto* requirements = object_field(root, "requires");
    std::vector<std::string> platforms;
    if (requirements) platforms = string_array_field(*requirements, "platforms");
    for (const auto& platform : platforms) {
        if (!valid_platform(platform)) {
            add_issue(result, "error", "invalid-platform",
                      "`requires.platforms` contains unsupported platform `" + platform + "`");
        }
    }

    const bool dynamic_native =
        vector_contains(kinds, "node-pack") || vector_contains(kinds, "native-component");
    if (dynamic_native
        && (vector_contains(platforms, "iOS") || vector_contains(platforms, "AUv3"))) {
        add_issue(result, "error", "dynamic-native-unsupported",
                  "Dynamic native loading packages cannot claim iOS or AUv3 support");
    }
}

void validate_path_array(KitValidationResult& result,
                         const fs::path& root,
                         const JsonValue& object,
                         const std::string& key) {
    auto* field = object.get(key);
    if (!field) return;
    if (field->type != JsonValue::Array) {
        add_issue(result, "error", "invalid-path-list", "`" + key + "` must be an array");
        return;
    }
    for (const auto& item : field->arr()) {
        if (item.type != JsonValue::String || item.str_val.empty()) {
            add_issue(result, "error", "invalid-path",
                      "`" + key + "` entries must be non-empty relative paths");
            continue;
        }
        if (!path_value_exists(root, item.str_val)) {
            add_issue(result, "error", "missing-path",
                      "`" + key + "` references missing or unsafe path `" + item.str_val + "`");
        }
    }
}

bool valid_sha256_digest(const std::string& digest) {
    if (digest.rfind("sha256-", 0) != 0) return false;
    if (digest.size() != std::string("sha256-").size() + 64) return false;
    return std::all_of(digest.begin() + 7, digest.end(), [](unsigned char c) {
        return std::isxdigit(c);
    });
}

void validate_evidence_item(KitValidationResult& result,
                            const fs::path& root,
                            const std::string& key,
                            const std::string& rel,
                            const std::string& digest) {
    if (rel.empty()) {
        add_issue(result, "error", "invalid-evidence",
                  "`evidence." + key + "` entries must declare a non-empty path");
        return;
    }
    if (!path_value_exists(root, rel)) {
        add_issue(result, "error", "missing-path",
                  "`evidence." + key + "` references missing or unsafe path `" + rel + "`");
        return;
    }
    if (digest.empty()) return;
    if (!valid_sha256_digest(digest)) {
        add_issue(result, "error", "invalid-evidence-digest",
                  "`evidence." + key + "` digest for `" + rel
                      + "` must be sha256- followed by 64 hex characters");
        return;
    }
    const auto bytes = read_bytes(root / fs::path(rel));
    const auto actual = "sha256-" + pulp::runtime::sha256_hex(bytes.data(), bytes.size());
    if (actual != digest) {
        add_issue(result, "error", "evidence-digest-mismatch",
                  "`evidence." + key + "` digest mismatch for `" + rel + "`");
    }
}

void validate_evidence_array(KitValidationResult& result,
                             const fs::path& root,
                             const JsonValue& object,
                             const std::string& key) {
    auto* field = object.get(key);
    if (!field) return;
    if (field->type != JsonValue::Array) {
        add_issue(result, "error", "invalid-evidence",
                  "`evidence." + key + "` must be an array");
        return;
    }
    for (const auto& item : field->arr()) {
        if (item.type == JsonValue::String) {
            validate_evidence_item(result, root, key, item.str_val, {});
            continue;
        }
        if (item.type == JsonValue::Object) {
            validate_evidence_item(result, root, key,
                                   string_field(item, "path"),
                                   string_field(item, "sha256"));
            continue;
        }
        add_issue(result, "error", "invalid-evidence",
                  "`evidence." + key
                      + "` entries must be relative paths or objects with path/sha256");
    }
}

bool field_array_empty(const JsonValue& object, const std::string& key) {
    auto* value = object.get(key);
    return !value || value->type != JsonValue::Array || value->arr().empty();
}

void validate_kind_required_evidence(KitValidationResult& result,
                                     const JsonValue& manifest) {
    auto* evidence = object_field(manifest, "evidence");
    auto* validation = object_field(manifest, "validation");
    if (vector_contains(result.summary.kinds, "template")) {
        const bool has_generated_project_diffs =
            validation && !field_array_empty(*validation, "generatedProjectDiffs");
        if (!has_generated_project_diffs) {
            add_issue(result, "error", "missing-template-generated-project-diff",
                      "Template kits must declare validation.generatedProjectDiffs review evidence");
        }
    }
    if (vector_contains(result.summary.kinds, "ui-kit")) {
        const bool has_screenshots =
            evidence && !field_array_empty(*evidence, "screenshots");
        const bool has_reports = evidence && !field_array_empty(*evidence, "reports");
        if (!has_screenshots && !has_reports) {
            add_issue(result, "error", "missing-ui-evidence",
                      "UI kits must declare screenshot/report evidence before validation can pass");
        }
    }
}

void validate_declared_paths(KitValidationResult& result,
                             const fs::path& root,
                             const JsonValue& manifest) {
    if (auto* exports = object_field(manifest, "exports")) {
        for (const auto* key : {"pulpUiScripts", "designTokens", "assets", "templates",
                                "validationReports", "screenshots", "presets", "themes",
                                "samples", "wavetables", "licenses", "sourceFiles",
                                "nativeComponentHeaders", "nativeComponentSources",
                                "nodePackManifests", "graphFixtures", "stateFixtures"}) {
            validate_path_array(result, root, *exports, key);
        }
        for (const auto& target : string_array_field(*exports, "cmakeTargets")) {
            if (!valid_cmake_target_name(target)) {
                add_issue(result, "error", "invalid-cmake-target",
                          "`exports.cmakeTargets` contains unsafe target name `" + target + "`");
            }
        }
    }
    if (auto* validation = object_field(manifest, "validation")) {
        validate_path_array(result, root, *validation, "profiles");
        validate_path_array(result, root, *validation, "reports");
        validate_path_array(result, root, *validation, "generatedProjectDiffs");
    }
    if (auto* evidence = object_field(manifest, "evidence")) {
        validate_evidence_array(result, root, *evidence, "screenshots");
        validate_evidence_array(result, root, *evidence, "reports");
        validate_evidence_array(result, root, *evidence, "validationReports");
    }
    validate_kind_required_evidence(result, manifest);
    if (auto* agent = object_field(manifest, "agent")) {
        if (auto guidance = string_field(*agent, "guidance"); !guidance.empty()) {
            if (!path_value_exists(root, guidance)) {
                add_issue(result, "error", "missing-path",
                          "`agent.guidance` references missing or unsafe path `" + guidance + "`");
            }
        }
        if (bool_field(*agent, "autoApply", false)) {
            add_issue(result, "error", "agent-auto-apply",
                      "`agent.autoApply` is not allowed; agent guidance is advisory only");
        }
    }
}

std::string authoring_creator_type(const JsonValue& authoring) {
    if (auto created_by = string_field(authoring, "createdBy"); !created_by.empty())
        return created_by;
    if (auto* created_by = object_field(authoring, "createdBy")) {
        return string_field(*created_by, "type");
    }
    return {};
}

bool valid_authoring_creator_type(const std::string& type) {
    return type.empty() || type == "human" || type == "agent" || type == "mixed";
}

bool authoring_human_reviewed(const JsonValue& authoring) {
    if (has_field(authoring, "humanReviewed"))
        return bool_field(authoring, "humanReviewed", false);
    if (auto* human_review = object_field(authoring, "humanReview")) {
        return bool_field(*human_review, "reviewed", false);
    }
    return false;
}

void validate_agent_authoring(KitValidationResult& result, const JsonValue& manifest) {
    auto* authoring = object_field(manifest, "authoring");
    if (!authoring) {
        add_issue(result, "warning", "missing-authoring",
                  "`authoring` block is recommended for provenance");
        return;
    }
    const auto creator_type = authoring_creator_type(*authoring);
    if (!valid_authoring_creator_type(creator_type)) {
        add_issue(result, "error", "invalid-authoring",
                  "`authoring.createdBy` must identify a human, agent, or mixed author");
    }
    if (creator_type == "agent" && !authoring_human_reviewed(*authoring)) {
        add_issue(result, "warning", "agent-review-required",
                  "agent-authored packages require recorded human review before publishing");
    }
}

std::string strings_json(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += ",";
        out += json_string(values[i]);
    }
    out += "]";
    return out;
}

std::string issues_json(const std::vector<KitIssue>& issues) {
    std::string out = "[";
    for (std::size_t i = 0; i < issues.size(); ++i) {
        if (i) out += ",";
        out += "{\"severity\":" + json_string(issues[i].severity)
            + ",\"code\":" + json_string(issues[i].code)
            + ",\"message\":" + json_string(issues[i].message) + "}";
    }
    out += "]";
    return out;
}

void add_publish_issue(KitValidationResult& result,
                       const std::string& severity,
                       const std::string& code,
                       const std::string& message) {
    result.issues.push_back({severity, code, message});
}

bool has_issue_code(const KitValidationResult& result, const std::string& code) {
    return std::any_of(result.issues.begin(), result.issues.end(), [&](const KitIssue& issue) {
        return issue.code == code;
    });
}

void validate_publish_policy(KitValidationResult& result, const JsonValue& manifest) {
    if (auto* authoring = object_field(manifest, "authoring")) {
        if (authoring_creator_type(*authoring) == "agent"
            && !authoring_human_reviewed(*authoring)) {
            add_publish_issue(result, "error", "agent-review-required",
                              "agent-authored packages cannot publish without recorded human review");
        }
    }

    if (auto* validation = object_field(manifest, "validation")) {
        if (field_array_empty(*validation, "profiles")) {
            add_publish_issue(result, "error", "missing-validation-profile",
                              "`validation.profiles` must name at least one validation profile before publishing");
        }
    }

    auto* exports = object_field(manifest, "exports");
    auto* evidence = object_field(manifest, "evidence");
    if (!exports || field_array_empty(*exports, "licenses")) {
        add_publish_issue(result, "error", "missing-notice-compatibility",
                          "`exports.licenses` must declare license/notice files before publishing");
    }
    if (vector_contains(result.summary.kinds, "ui-kit")) {
        const bool has_screenshots =
            evidence && !field_array_empty(*evidence, "screenshots");
        const bool has_reports = evidence && !field_array_empty(*evidence, "reports");
        if (!has_screenshots && !has_reports) {
            add_publish_issue(result, "error", "missing-ui-evidence",
                              "UI kits must declare screenshot/report evidence before publishing");
        }
    }
    if (vector_contains(result.summary.kinds, "node-pack")
        && (!exports || field_array_empty(*exports, "nodePackManifests"))) {
        add_publish_issue(result, "error", "missing-node-pack-manifest",
                          "node-pack kits must export at least one nodePackManifests entry");
    }
    if (vector_contains(result.summary.kinds, "native-component")) {
        const bool has_headers = exports && !field_array_empty(*exports, "nativeComponentHeaders");
        const bool has_sources = exports && !field_array_empty(*exports, "nativeComponentSources");
        if (!has_headers && !has_sources) {
            add_publish_issue(result, "error", "missing-native-component-files",
                              "native-component kits must export headers or sources before publishing");
        }
    }
}

std::string registry_manifest_signed_message(const KitSummary& summary,
                                             const std::string& canonical_sha256) {
    return "pulp-registry-manifest-v1\n"
        + summary.id + "\n"
        + summary.version + "\n"
        + canonical_sha256 + "\n";
}

void validate_registry_manifest(KitValidationResult& result,
                                const JsonValue& registry_manifest,
                                const std::string& expected_sha) {
    if (registry_manifest.type != JsonValue::Object) {
        add_publish_issue(result, "error", "invalid-registry-manifest",
                          "registry manifest must be a JSON object");
        return;
    }
    if (string_field(registry_manifest, "schema") != "pulp-registry-manifest-v1") {
        add_publish_issue(result, "error", "invalid-registry-manifest",
                          "registry manifest schema must be `pulp-registry-manifest-v1`");
    }
    if (string_field(registry_manifest, "id") != result.summary.id) {
        add_publish_issue(result, "error", "registry-manifest-id-mismatch",
                          "registry manifest id must match pulp.package.json");
    }
    if (string_field(registry_manifest, "version") != result.summary.version) {
        add_publish_issue(result, "error", "registry-manifest-version-mismatch",
                          "registry manifest version must match pulp.package.json");
    }

    const auto canonical_sha = string_field(registry_manifest, "canonicalManifestSha256");
    if (canonical_sha != expected_sha) {
        add_publish_issue(result, "error", "registry-manifest-digest-mismatch",
                          "registry manifest canonicalManifestSha256 must match pulp.package.json bytes");
    }

    auto public_key = hex_decode_local(string_field(registry_manifest, "signerPublicKey"));
    auto signature = hex_decode_local(string_field(registry_manifest, "signature"));
    if (public_key.size() != pulp::runtime::ed25519_public_key_size) {
        add_publish_issue(result, "error", "registry-manifest-public-key-invalid",
                          "registry manifest signerPublicKey must be a 32-byte Ed25519 key encoded as hex");
    }
    if (signature.size() != pulp::runtime::ed25519_signature_size) {
        add_publish_issue(result, "error", "registry-manifest-signature-invalid",
                          "registry manifest signature must be a 64-byte Ed25519 signature encoded as hex");
    }
    if (public_key.size() == pulp::runtime::ed25519_public_key_size
        && signature.size() == pulp::runtime::ed25519_signature_size
        && !canonical_sha.empty()) {
        const auto message = registry_manifest_signed_message(result.summary, canonical_sha);
        if (!pulp::runtime::ed25519_verify(public_key.data(), public_key.size(),
                                           signature.data(), signature.size(),
                                           reinterpret_cast<const std::uint8_t*>(message.data()),
                                           message.size())) {
            add_publish_issue(result, "error", "registry-manifest-signature-mismatch",
                              "registry manifest signature must verify the canonical package digest");
        }
    }
}

std::string publish_badges_json(const KitValidationResult& result,
                                bool registry_manifest_checked) {
    std::vector<std::string> badges;
    if (result.ok()) badges.push_back("publish-ready");
    if (!has_issue_code(result, "missing-license-inventory")) badges.push_back("license-inventory");
    if (!has_issue_code(result, "missing-notice-compatibility")) badges.push_back("notice-compatibility");
    if (!has_issue_code(result, "agent-review-required")) badges.push_back("human-review");
    if (!has_issue_code(result, "missing-validation-profile")) badges.push_back("validation-profiles");
    if (!has_issue_code(result, "missing-ui-evidence")
        && !has_issue_code(result, "missing-node-pack-manifest")
        && !has_issue_code(result, "missing-native-component-files")) {
        badges.push_back("kind-evidence");
    }
    if (registry_manifest_checked
        && !has_issue_code(result, "missing-registry-manifest")
        && !has_issue_code(result, "invalid-registry-manifest")
        && !has_issue_code(result, "registry-manifest-id-mismatch")
        && !has_issue_code(result, "registry-manifest-version-mismatch")
        && !has_issue_code(result, "registry-manifest-digest-mismatch")
        && !has_issue_code(result, "registry-manifest-public-key-invalid")
        && !has_issue_code(result, "registry-manifest-signature-invalid")
        && !has_issue_code(result, "registry-manifest-signature-mismatch")) {
        badges.push_back("signed-canonical-manifest");
    }
    return strings_json(badges);
}

std::string compatibility_json(const JsonValue& manifest) {
    const auto* requirements = object_field(manifest, "requires");
    const auto pulp_req = requirements ? string_field(*requirements, "pulp") : std::string{};
    const auto cpp = requirements ? int_field(*requirements, "cpp", 0) : 0;
    const auto platforms = requirements ? string_array_field(*requirements, "platforms") : std::vector<std::string>{};
    int platform_score = static_cast<int>(platforms.size()) * 100 / 7;
    if (platform_score > 100) platform_score = 100;
    return "{\"requires_pulp\":" + json_string(pulp_req)
        + ",\"minimum_cpp\":" + std::to_string(cpp)
        + ",\"platforms\":" + strings_json(platforms)
        + ",\"platform_score\":" + std::to_string(platform_score)
        + "}";
}

std::string summary_json(const KitSummary& summary) {
    return "{\"manifest_path\":" + json_string(summary.manifest_path.string())
        + ",\"root\":" + json_string(summary.root.string())
        + ",\"schema\":" + json_string(summary.schema)
        + ",\"id\":" + json_string(summary.id)
        + ",\"name\":" + json_string(summary.name)
        + ",\"version\":" + json_string(summary.version)
        + ",\"license\":" + json_string(summary.license)
        + ",\"kind\":" + strings_json(summary.kinds)
        + ",\"capabilities\":" + strings_json(summary.capabilities)
        + ",\"dependency_packages\":" + strings_json(summary.dependency_packages)
        + "}";
}

struct KitPlanAction {
    std::string kind;
    std::string path;
    std::string description;
    std::string command;
};

struct KitProfileResult {
    std::string path;
    std::string kind;
    std::string status;
    std::vector<std::pair<std::string, std::string>> artifacts;
    std::vector<KitIssue> issues;
};

struct KitVerifyOptions {
    bool execute_screenshots = false;
    fs::path screenshot_output_dir;
    std::string screenshot_backend = "auto";
};

struct KitLockEntry {
    std::string id;
    std::string version;
    std::string source;
    std::string manifest_sha256;
    std::vector<std::string> kinds;
    std::vector<std::string> dependency_packages;
    std::vector<std::string> cmake_targets;
    std::vector<std::string> ui_scripts;
    std::vector<std::string> design_tokens;
    std::vector<std::string> assets;
    std::vector<std::string> source_files;
    std::vector<std::string> native_component_headers;
    std::vector<std::string> native_component_sources;
    std::vector<std::string> node_pack_manifests;
    std::vector<std::string> graph_fixtures;
    std::vector<std::string> state_fixtures;
    std::vector<std::string> owned_paths;
};

std::string sanitize_id_for_cmake(const std::string& id) {
    std::string out = "pulp_kit_";
    for (char c : id) {
        const unsigned char u = static_cast<unsigned char>(c);
        out += std::isalnum(u) ? c : '_';
    }
    return out;
}

std::vector<std::string> string_array_member(const JsonValue& object, const std::string& key) {
    return string_array_field(object, key);
}

std::string cmake_quoted_list(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += ";";
        for (char c : value) {
            if (c == '\\' || c == '"') out += '\\';
            out += c;
        }
    }
    return out;
}

std::string copied_kit_rel_path(const std::string& kit_id, const std::string& rel) {
    return (fs::path("pulp-kits") / kit_id / fs::path(rel)).generic_string();
}

std::vector<KitLockEntry> load_kit_lock(const fs::path& path) {
    std::vector<KitLockEntry> entries;
    if (!fs::exists(path)) return entries;
    auto root = parse_manifest_json(path);
    if (root.type != JsonValue::Object) return entries;
    auto* kits = object_field(root, "kits");
    if (!kits) return entries;
    for (const auto& [id, value] : kits->obj()) {
        if (value.type != JsonValue::Object) continue;
        KitLockEntry entry;
        entry.id = id;
        entry.version = string_field(value, "version");
        entry.source = string_field(value, "source");
        entry.manifest_sha256 = string_field(value, "manifest_sha256");
        entry.kinds = string_array_member(value, "kind");
        entry.dependency_packages = string_array_member(value, "dependency_packages");
        entry.cmake_targets = string_array_member(value, "cmake_targets");
        entry.ui_scripts = string_array_member(value, "ui_scripts");
        entry.design_tokens = string_array_member(value, "design_tokens");
        entry.assets = string_array_member(value, "assets");
        entry.source_files = string_array_member(value, "source_files");
        entry.native_component_headers = string_array_member(value, "native_component_headers");
        entry.native_component_sources = string_array_member(value, "native_component_sources");
        entry.node_pack_manifests = string_array_member(value, "node_pack_manifests");
        entry.graph_fixtures = string_array_member(value, "graph_fixtures");
        entry.state_fixtures = string_array_member(value, "state_fixtures");
        entry.owned_paths = string_array_member(value, "owned_paths");
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::string kit_lock_json(const std::vector<KitLockEntry>& entries) {
    std::string out = "{\n  \"version\": 1,\n  \"kits\": {";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        out += i == 0 ? "\n" : ",\n";
        out += "    " + json_string(entry.id) + ": {\n";
        out += "      \"version\": " + json_string(entry.version) + ",\n";
        out += "      \"source\": " + json_string(entry.source) + ",\n";
        if (!entry.manifest_sha256.empty()) {
            out += "      \"manifest_sha256\": " + json_string(entry.manifest_sha256) + ",\n";
        }
        out += "      \"kind\": " + strings_json(entry.kinds) + ",\n";
        out += "      \"dependency_packages\": " + strings_json(entry.dependency_packages) + ",\n";
        out += "      \"cmake_targets\": " + strings_json(entry.cmake_targets) + ",\n";
        out += "      \"ui_scripts\": " + strings_json(entry.ui_scripts) + ",\n";
        out += "      \"design_tokens\": " + strings_json(entry.design_tokens) + ",\n";
        out += "      \"assets\": " + strings_json(entry.assets) + ",\n";
        out += "      \"source_files\": " + strings_json(entry.source_files) + ",\n";
        out += "      \"native_component_headers\": " + strings_json(entry.native_component_headers) + ",\n";
        out += "      \"native_component_sources\": " + strings_json(entry.native_component_sources) + ",\n";
        out += "      \"node_pack_manifests\": " + strings_json(entry.node_pack_manifests) + ",\n";
        out += "      \"graph_fixtures\": " + strings_json(entry.graph_fixtures) + ",\n";
        out += "      \"state_fixtures\": " + strings_json(entry.state_fixtures) + ",\n";
        out += "      \"owned_paths\": " + strings_json(entry.owned_paths) + "\n";
        out += "    }";
    }
    out += entries.empty() ? "\n  }\n}\n" : "\n  }\n}\n";
    return out;
}

std::string generated_cmake(const std::vector<KitLockEntry>& entries) {
    std::ostringstream out;
    out << "# Generated by pulp kit apply. Do not edit by hand.\n\n";
    for (const auto& entry : entries) {
        const auto target = sanitize_id_for_cmake(entry.id);
        out << "# BEGIN_PULP_KIT " << entry.id << "\n";
        out << "add_library(" << target << " INTERFACE)\n";
        if (!entry.cmake_targets.empty()) {
            out << "# Declared exported CMake targets:";
            for (const auto& target : entry.cmake_targets) out << " " << target;
            out << "\n";
        }
        if (!entry.ui_scripts.empty() || !entry.design_tokens.empty() || !entry.assets.empty()
            || !entry.source_files.empty() || !entry.native_component_headers.empty()
            || !entry.native_component_sources.empty() || !entry.node_pack_manifests.empty()
            || !entry.graph_fixtures.empty() || !entry.state_fixtures.empty()) {
            out << "set_property(TARGET " << target << " PROPERTY PULP_KIT_ID "
                << json_string(entry.id) << ")\n";
            if (!entry.ui_scripts.empty()) {
                out << "set_property(TARGET " << target << " PROPERTY PULP_UI_SCRIPTS "
                    << json_string(cmake_quoted_list(entry.ui_scripts)) << ")\n";
            }
            if (!entry.design_tokens.empty()) {
                out << "set_property(TARGET " << target << " PROPERTY PULP_DESIGN_TOKENS "
                    << json_string(cmake_quoted_list(entry.design_tokens)) << ")\n";
            }
            if (!entry.assets.empty()) {
                out << "set_property(TARGET " << target << " PROPERTY PULP_ASSETS "
                    << json_string(cmake_quoted_list(entry.assets)) << ")\n";
            }
            if (!entry.source_files.empty()) {
                out << "set_property(TARGET " << target << " PROPERTY PULP_SOURCE_FILES "
                    << json_string(cmake_quoted_list(entry.source_files)) << ")\n";
            }
            if (!entry.native_component_headers.empty()) {
                out << "set_property(TARGET " << target << " PROPERTY PULP_NATIVE_COMPONENT_HEADERS "
                    << json_string(cmake_quoted_list(entry.native_component_headers)) << ")\n";
            }
            if (!entry.native_component_sources.empty()) {
                out << "set_property(TARGET " << target << " PROPERTY PULP_NATIVE_COMPONENT_SOURCES "
                    << json_string(cmake_quoted_list(entry.native_component_sources)) << ")\n";
            }
            if (!entry.node_pack_manifests.empty()) {
                out << "set_property(TARGET " << target << " PROPERTY PULP_NODE_PACK_MANIFESTS "
                    << json_string(cmake_quoted_list(entry.node_pack_manifests)) << ")\n";
            }
            if (!entry.graph_fixtures.empty()) {
                out << "set_property(TARGET " << target << " PROPERTY PULP_GRAPH_FIXTURES "
                    << json_string(cmake_quoted_list(entry.graph_fixtures)) << ")\n";
            }
            if (!entry.state_fixtures.empty()) {
                out << "set_property(TARGET " << target << " PROPERTY PULP_STATE_FIXTURES "
                    << json_string(cmake_quoted_list(entry.state_fixtures)) << ")\n";
            }
        }
        out << "# END_PULP_KIT " << entry.id << "\n\n";
    }
    return out.str();
}

bool ensure_cmake_include(const fs::path& project_root) {
    auto cmakelists = project_root / "CMakeLists.txt";
    auto text = read_text(cmakelists);
    if (text.empty()) return false;
    const std::string include_line = "include(cmake/pulp-kits.cmake OPTIONAL)";
    if (text.find(include_line) != std::string::npos) return true;
    if (!text.empty() && text.back() != '\n') text += "\n";
    text += include_line + "\n";
    return write_text(cmakelists, text);
}

bool remove_cmake_include_if_unused(const fs::path& project_root) {
    auto cmakelists = project_root / "CMakeLists.txt";
    auto text = read_text(cmakelists);
    if (text.empty()) return false;
    const std::string include_line = "include(cmake/pulp-kits.cmake OPTIONAL)";
    auto pos = text.find(include_line);
    if (pos == std::string::npos) return true;
    auto erase_end = pos + include_line.size();
    if (erase_end < text.size() && text[erase_end] == '\r') ++erase_end;
    if (erase_end < text.size() && text[erase_end] == '\n') ++erase_end;
    text.erase(pos, erase_end - pos);
    return write_text(cmakelists, text);
}

bool remove_owned_path(const fs::path& project_root,
                       const std::string& rel,
                       std::string& error) {
    if (rel.empty() || rel == ".pulp/kits.lock.json" || rel == "cmake/pulp-kits.cmake")
        return true;
    fs::path rel_path(rel);
    if (rel_path.is_absolute()) {
        error = "Refusing to remove absolute owned path `" + rel + "`";
        return false;
    }
    std::error_code ec;
    auto root = fs::weakly_canonical(project_root, ec);
    if (ec) root = project_root.lexically_normal();
    auto target = fs::weakly_canonical(project_root / rel_path, ec);
    if (ec) target = (project_root / rel_path).lexically_normal();
    if (!path_is_within_local(target, root)) {
        error = "Refusing to remove unsafe owned path `" + rel + "`";
        return false;
    }
    const auto normalized_rel = rel_path.lexically_normal();
    auto first = normalized_rel.begin();
    if (first == normalized_rel.end() || first->string() != "pulp-kits") {
        error = "Refusing to remove non-kit owned path `" + rel + "`";
        return false;
    }
    if (!fs::exists(target)) return true;
    fs::remove_all(target, ec);
    if (ec) {
        error = "Failed to remove `" + rel + "`: " + ec.message();
        return false;
    }
    return true;
}

void prune_empty_parent_dirs(const fs::path& project_root, fs::path rel) {
    std::error_code ec;
    while (!rel.empty() && rel != "." && rel != "pulp-kits") {
        auto dir = project_root / rel;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec) || !fs::is_empty(dir, ec))
            break;
        fs::remove(dir, ec);
        rel = rel.parent_path();
    }
}

bool reject_symlink_tree(const fs::path& source,
                         const std::string& rel,
                         std::string& error) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(source, ec))) {
        error = "Symlinks are not allowed in kit exports: `" + rel + "`";
        return false;
    }
    if (ec) {
        error = "Failed to inspect kit path `" + rel + "`: " + ec.message();
        return false;
    }
    if (!fs::is_directory(source, ec)) return true;
    if (ec) {
        error = "Failed to inspect kit path `" + rel + "`: " + ec.message();
        return false;
    }

    for (fs::recursive_directory_iterator it(source, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            error = "Failed to inspect kit path `" + rel + "`: " + ec.message();
            return false;
        }
        if (fs::is_symlink(it->symlink_status(ec))) {
            auto path = it->path().lexically_relative(source).generic_string();
            error = "Symlinks are not allowed in kit exports: `"
                + (fs::path(rel) / path).generic_string() + "`";
            return false;
        }
        if (ec) {
            error = "Failed to inspect kit path `" + rel + "`: " + ec.message();
            return false;
        }
    }
    return true;
}

bool copy_declared_path(const fs::path& kit_root,
                        const fs::path& project_root,
                        const std::string& kit_id,
                        const std::string& rel,
                        std::vector<std::string>& owned_paths,
                        std::vector<std::string>* rollback_paths,
                        std::string& error) {
    fs::path rel_path(rel);
    if (rel_path.is_absolute()) {
        error = "Refusing to copy absolute kit path `" + rel + "`";
        return false;
    }
    std::error_code ec;
    auto source = fs::weakly_canonical(kit_root / rel_path, ec);
    if (ec) source = (kit_root / rel_path).lexically_normal();
    auto root = fs::weakly_canonical(kit_root, ec);
    if (ec) root = kit_root.lexically_normal();
    if (!path_is_within_local(source, root) || !fs::exists(source)) {
        error = "Refusing to copy missing or unsafe kit path `" + rel + "`";
        return false;
    }
    if (!reject_symlink_tree(kit_root / rel_path, rel, error)) {
        return false;
    }

    auto dest = project_root / "pulp-kits" / kit_id / rel_path;
    const bool dest_existed = fs::exists(dest, ec);
    ec.clear();
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        error = "Failed to create " + dest.parent_path().string();
        return false;
    }
    if (fs::is_directory(source)) {
        fs::create_directories(dest, ec);
        if (ec) {
            error = "Failed to create " + dest.string();
            return false;
        }
        fs::copy(source, dest,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
    } else {
        fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
        error = "Failed to copy `" + rel + "`: " + ec.message();
        return false;
    }
    auto owned = fs::relative(dest, project_root, ec).string();
    owned_paths.push_back(owned);
    if (!dest_existed && rollback_paths) {
        rollback_paths->push_back(owned);
    }
    return true;
}

struct FileSnapshot {
    bool existed = false;
    std::string contents;
};

FileSnapshot snapshot_file(const fs::path& path) {
    FileSnapshot snapshot;
    snapshot.existed = fs::exists(path);
    if (snapshot.existed) snapshot.contents = read_text(path);
    return snapshot;
}

void restore_file_snapshot(const fs::path& path, const FileSnapshot& snapshot) {
    std::error_code ec;
    if (snapshot.existed) {
        write_text(path, snapshot.contents);
    } else {
        fs::remove(path, ec);
        prune_empty_parent_dirs(path.parent_path().parent_path(), path.parent_path().filename());
    }
}

void rollback_apply_files(const fs::path& project_root,
                          const std::vector<std::string>& rollback_paths) {
    std::string ignored;
    for (auto it = rollback_paths.rbegin(); it != rollback_paths.rend(); ++it) {
        remove_owned_path(project_root, *it, ignored);
        prune_empty_parent_dirs(project_root, fs::path(*it).parent_path());
        ignored.clear();
    }
}

std::string sha256_file_hex_local(const fs::path& path) {
    const auto bytes = read_bytes(path);
    if (bytes.empty() && fs::file_size(path) != 0) return {};
    return pulp::runtime::sha256_hex(bytes.data(), bytes.size());
}

bool safe_archive_rel(const fs::path& rel) {
    if (rel.empty() || rel.is_absolute()) return false;
    for (const auto& part : rel) {
        const auto s = part.string();
        if (s.empty() || s == "." || s == "..") return false;
    }
    return true;
}

bool is_package_archive_path(const fs::path& path) {
    const auto ext = path.extension().string();
    return ext == ".pulpkit" || ext == ".pulpcontent";
}

fs::path temporary_archive_root() {
    static std::atomic<unsigned> seq{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("pulp-kit-archive-" + std::to_string(ticks) + "-" +
            std::to_string(seq.fetch_add(1)));
}

bool zip_read_file(mz_zip_archive& zip, const char* name, std::string& out) {
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, name, &size, 0);
    if (!data) return false;
    out.assign(static_cast<const char*>(data), static_cast<const char*>(data) + size);
    mz_free(data);
    return true;
}

bool write_binary_file(const fs::path& path, const void* data, std::size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return f.good();
}

bool validate_and_extract_kit_archive(const fs::path& archive,
                                      const fs::path& dest_root,
                                      std::vector<std::string>& issues) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive.string().c_str(), 0)) {
        issues.push_back("archive-open: failed to open " + archive.string());
        return false;
    }

    bool ok = true;
    std::vector<std::string> archived_payloads;
    const auto count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            issues.push_back("archive-entry: failed to read entry metadata");
            ok = false;
            continue;
        }
        const fs::path rel(stat.m_filename);
        if (!safe_archive_rel(rel)) {
            issues.push_back("archive-entry: unsafe path `" + rel.generic_string() + "`");
            ok = false;
        }
        if (!stat.m_is_directory && rel.generic_string() != "files.sha256.json")
            archived_payloads.push_back(rel.generic_string());
    }

    std::string sha_json;
    if (!zip_read_file(zip, "files.sha256.json", sha_json)) {
        issues.push_back("sha256: missing files.sha256.json");
        ok = false;
    } else {
        JsonParser parser{sha_json};
        const auto sha_root = parser.parse();
        auto* files = object_field(sha_root, "files");
        if (sha_root.type != JsonValue::Object || !files || files->type != JsonValue::Object) {
            issues.push_back("sha256: invalid files.sha256.json");
            ok = false;
        } else {
            std::vector<std::string> declared_files;
            for (const auto& [name, value] : files->obj()) {
                if (value.type != JsonValue::String || !safe_archive_rel(fs::path(name))) {
                    issues.push_back("sha256: invalid entry `" + name + "`");
                    ok = false;
                    continue;
                }
                declared_files.push_back(name);
                size_t size = 0;
                void* data = mz_zip_reader_extract_file_to_heap(&zip, name.c_str(), &size, 0);
                if (!data) {
                    issues.push_back("sha256: missing archived file `" + name + "`");
                    ok = false;
                    continue;
                }
                const auto hash = pulp::runtime::sha256_hex(
                    static_cast<const unsigned char*>(data), size);
                mz_free(data);
                if (value.str_val != "sha256-" + hash) {
                    issues.push_back("sha256: digest mismatch for `" + name + "`");
                    ok = false;
                }
            }
            for (const auto& name : archived_payloads) {
                if (std::find(declared_files.begin(), declared_files.end(), name)
                    == declared_files.end()) {
                    issues.push_back("sha256: unlisted archived file `" + name + "`");
                    ok = false;
                }
            }
        }
    }

    if (!ok) {
        mz_zip_reader_end(&zip);
        return false;
    }

    std::error_code ec;
    fs::create_directories(dest_root, ec);
    const auto root_norm = fs::absolute(dest_root, ec).lexically_normal();
    if (ec) {
        issues.push_back("archive-extract: failed to create temp root");
        mz_zip_reader_end(&zip);
        return false;
    }

    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            issues.push_back("archive-entry: failed to read entry metadata");
            ok = false;
            continue;
        }
        if (stat.m_is_directory) continue;
        const fs::path rel(stat.m_filename);
        const auto dest = (dest_root / rel).lexically_normal();
        const auto dest_abs = fs::absolute(dest, ec).lexically_normal();
        if (ec || !path_is_within_local(dest_abs, root_norm)) {
            issues.push_back("archive-entry: unsafe extraction path `" + rel.generic_string() + "`");
            ok = false;
            continue;
        }
        size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&zip, stat.m_filename, &size, 0);
        if (!data) {
            issues.push_back("archive-extract: failed to extract `" + rel.generic_string() + "`");
            ok = false;
            continue;
        }
        if (!write_binary_file(dest, data, size)) {
            issues.push_back("archive-extract: failed to write `" + rel.generic_string() + "`");
            ok = false;
        }
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

void add_archive_issue(KitValidationResult& result, const std::string& issue) {
    const auto pos = issue.find(':');
    if (pos == std::string::npos) {
        add_issue(result, "error", "archive", issue);
        return;
    }
    auto message = issue.substr(pos + 1);
    while (!message.empty() && message.front() == ' ')
        message.erase(message.begin());
    add_issue(result, "error", issue.substr(0, pos), message);
}

struct KitInputWorkspace {
    fs::path original;
    fs::path manifest_path;
    fs::path temp_root;
    bool archive = false;
    std::vector<std::string> archive_issues;

    explicit KitInputWorkspace(fs::path input) : original(std::move(input)) {
        if (is_package_archive_path(original)) {
            archive = true;
            temp_root = temporary_archive_root();
            if (validate_and_extract_kit_archive(original, temp_root, archive_issues))
                manifest_path = temp_root / "pulp.package.json";
        } else {
            manifest_path = resolve_manifest_path(original);
        }
    }

    KitInputWorkspace(const KitInputWorkspace&) = delete;
    KitInputWorkspace& operator=(const KitInputWorkspace&) = delete;

    ~KitInputWorkspace() {
        if (!temp_root.empty()) {
            std::error_code ec;
            fs::remove_all(temp_root, ec);
        }
    }
};

KitValidationResult validate_kit_input(KitInputWorkspace& input, bool strict) {
    if (!input.archive_issues.empty()) {
        KitValidationResult result;
        result.summary.manifest_path = input.original;
        result.summary.root = input.original.parent_path();
        for (const auto& issue : input.archive_issues)
            add_archive_issue(result, issue);
        return result;
    }
    return validate_manifest_path(input.manifest_path, strict);
}

std::string kit_source_label(const KitInputWorkspace& input,
                             const KitValidationResult& result) {
    return input.archive ? input.original.generic_string() : result.summary.root.generic_string();
}

bool collect_pack_files(const fs::path& root,
                        std::vector<fs::path>& files,
                        std::string& error) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (ec) {
            error = "Failed to inspect package path: " + ec.message();
            return false;
        }
        if (fs::is_symlink(it->symlink_status(ec))) {
            auto rel = it->path().lexically_relative(root);
            if (rel.empty()) rel = it->path().filename();
            error = "Symlinks are not allowed in package archives: `" + rel.generic_string() + "`";
            return false;
        }
        if (!it->is_regular_file(ec)) continue;
        auto rel = fs::relative(it->path(), root, ec);
        if (ec || !safe_archive_rel(rel)) {
            error = "Unsafe package archive path: `" + rel.generic_string() + "`";
            return false;
        }
        if (rel == "files.sha256.json") continue;
        files.push_back(rel.lexically_normal());
    }
    if (ec) {
        error = "Failed to inspect package path: " + ec.message();
        return false;
    }
    std::sort(files.begin(), files.end());
    return true;
}

std::string sha_manifest_json(const fs::path& root, const std::vector<fs::path>& files) {
    std::string out = "{\n  \"schema\": \"pulp-files-sha256-v1\",\n  \"files\": {";
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto rel = files[i].generic_string();
        const auto sha = sha256_file_hex_local(root / files[i]);
        out += i == 0 ? "\n" : ",\n";
        out += "    " + json_string(rel) + ": \"sha256-" + sha + "\"";
    }
    out += files.empty() ? "\n  }\n}\n" : "\n  }\n}\n";
    return out;
}

fs::path default_pack_output(const KitSummary& summary) {
    const auto extension = vector_contains(summary.kinds, "content-pack") ? ".pulpcontent" : ".pulpkit";
    return fs::path(summary.id + "-" + summary.version + extension);
}

bool write_zip_archive(const fs::path& root,
                       const fs::path& output,
                       const std::vector<fs::path>& files,
                       const std::string& sha_manifest,
                       std::string& error) {
    std::error_code ec;
    fs::create_directories(output.parent_path().empty() ? fs::current_path() : output.parent_path(), ec);
    if (ec) {
        error = "Failed to create output directory: " + ec.message();
        return false;
    }

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, output.string().c_str(), 0)) {
        error = "Failed to create archive " + output.string();
        return false;
    }

    for (const auto& rel : files) {
        const auto archive_name = rel.generic_string();
        if (!mz_zip_writer_add_file(&zip, archive_name.c_str(), (root / rel).string().c_str(),
                                    nullptr, 0, MZ_DEFAULT_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            error = "Failed to add `" + archive_name + "` to archive";
            return false;
        }
    }
    if (!mz_zip_writer_add_mem(&zip, "files.sha256.json",
                               sha_manifest.data(), sha_manifest.size(),
                               MZ_DEFAULT_COMPRESSION)) {
        mz_zip_writer_end(&zip);
        error = "Failed to add files.sha256.json to archive";
        return false;
    }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        error = "Failed to finalize archive " + output.string();
        return false;
    }
    mz_zip_writer_end(&zip);
    return true;
}

std::string action_json(const KitPlanAction& action) {
    std::string out = "{\"kind\":" + json_string(action.kind)
        + ",\"path\":" + json_string(action.path)
        + ",\"description\":" + json_string(action.description);
    if (!action.command.empty()) out += ",\"command\":" + json_string(action.command);
    out += "}";
    return out;
}

std::string actions_json(const std::vector<KitPlanAction>& actions) {
    std::string out = "[";
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (i) out += ",";
        out += action_json(actions[i]);
    }
    out += "]";
    return out;
}

std::string profile_result_json(const KitProfileResult& profile) {
    std::string artifacts = "[";
    for (std::size_t i = 0; i < profile.artifacts.size(); ++i) {
        if (i) artifacts += ",";
        artifacts += "{\"kind\":" + json_string(profile.artifacts[i].first)
            + ",\"path\":" + json_string(profile.artifacts[i].second) + "}";
    }
    artifacts += "]";
    return "{\"path\":" + json_string(profile.path)
        + ",\"kind\":" + json_string(profile.kind)
        + ",\"status\":" + json_string(profile.status)
        + ",\"artifacts\":" + artifacts
        + ",\"issues\":" + issues_json(profile.issues) + "}";
}

std::string profile_results_json(const std::vector<KitProfileResult>& profiles) {
    std::string out = "[";
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (i) out += ",";
        out += profile_result_json(profiles[i]);
    }
    out += "]";
    return out;
}

void add_profile_issue(KitProfileResult& profile,
                       std::string severity,
                       std::string code,
                       std::string message) {
    profile.issues.push_back({std::move(severity), std::move(code), std::move(message)});
}

bool profile_ok(const KitProfileResult& profile) {
    return std::none_of(profile.issues.begin(), profile.issues.end(), [](const KitIssue& issue) {
        return issue.severity == "error";
    });
}

bool manifest_exports_path(const JsonValue& manifest,
                           const std::string& export_key,
                           const std::string& rel_path);

bool string_array_contains(const JsonValue& object,
                           const std::string& key,
                           const std::string& needle) {
    const auto values = string_array_field(object, key);
    return std::find(values.begin(), values.end(), needle) != values.end();
}

bool valid_hex_string(std::string_view value, std::size_t expected_chars) {
    if (value.size() != expected_chars) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool graph_contains_node_type(const JsonValue& graph, const std::string& node_type) {
    auto* nodes = array_field(graph, "nodes");
    if (!nodes) return false;
    for (const auto& node : nodes->arr()) {
        if (node.type == JsonValue::Object && string_field(node, "type") == node_type)
            return true;
    }
    return false;
}

void verify_graph_fixture_shape(KitProfileResult& profile,
                                const JsonValue& graph_json) {
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
            if (node.type != JsonValue::Object || string_field(node, "id").empty()
                || string_field(node, "type").empty()) {
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

void verify_state_fixture_shape(KitProfileResult& profile,
                                const JsonValue& state_json) {
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

KitProfileResult verify_signal_graph_state_profile(const fs::path& root,
                                                   const JsonValue& manifest,
                                                   const fs::path& profile_rel,
                                                   const JsonValue& profile_json) {
    KitProfileResult profile;
    profile.path = profile_rel.generic_string();
    profile.kind = "signal-graph-state-validation";
    profile.status = "pass";

    for (const auto* check : {"register-custom-node-type", "save-load-state",
                              "reprepare", "process-no-alloc-no-lock"}) {
        if (!string_array_contains(profile_json, "checks", check)) {
            add_profile_issue(profile, "error", "missing-graph-state-check",
                              std::string("Graph/state profile must declare check `")
                                  + check + "`");
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
    if (!node_type.empty() && graph_json.type == JsonValue::Object
        && !graph_contains_node_type(graph_json, node_type)) {
        add_profile_issue(profile, "error", "graph-state-node-mismatch",
                          "State fixture nodeType must appear in graph fixture nodes");
    }

    if (!profile_ok(profile)) profile.status = "fail";
    return profile;
}

KitProfileResult verify_node_pack_profile(const fs::path& root,
                                          const JsonValue& manifest,
                                          const fs::path& profile_rel,
                                          const JsonValue& profile_json) {
    KitProfileResult profile;
    profile.path = profile_rel.generic_string();
    profile.kind = "node-pack-validation-profile";
    profile.status = "pass";

    for (const auto* check : {"manifest-shape", "signature-before-load", "hash-before-load"}) {
        if (!string_array_contains(profile_json, "checks", check)) {
            add_profile_issue(profile, "error", "missing-node-pack-check",
                              std::string("Node-pack profile must declare check `")
                                  + check + "`");
        }
    }
    if (bool_field(profile_json, "executeDuringInspect", true)) {
        add_profile_issue(profile, "error", "node-pack-executes-during-inspect",
                          "Node-pack profiles must set executeDuringInspect to false");
    }

    const auto manifest_rel = string_field(profile_json, "manifest");
    if (manifest_rel.empty() || !manifest_exports_path(manifest, "nodePackManifests", manifest_rel)) {
        add_profile_issue(profile, "error", "node-pack-manifest-not-exported",
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
                    if (node.type != JsonValue::Object || string_field(node, "type_id").empty()
                        || node.get("capabilities") == nullptr
                        || node.get("capabilities")->type != JsonValue::Number) {
                        add_profile_issue(profile, "error", "invalid-node-pack-nodes",
                                          "Node-pack nodes must declare type_id and numeric capabilities");
                        break;
                    }
                }
            }
        }
    }

    if (!profile_ok(profile)) profile.status = "fail";
    return profile;
}

bool extension_in(const fs::path& path, std::initializer_list<std::string_view> extensions) {
    const auto ext = path.extension().string();
    return std::any_of(extensions.begin(), extensions.end(), [&](std::string_view expected) {
        return ext == expected;
    });
}

KitProfileResult verify_native_component_profile(const fs::path& root,
                                                 const JsonValue& manifest,
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
                              std::string("Native-component profile must declare check `")
                                  + check + "`");
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
        if (!path_value_exists(root, header)
            || !extension_in(header, {".h", ".hpp", ".hh", ".hxx"})) {
            add_profile_issue(profile, "error", "invalid-native-component-header",
                              "Native-component headers must be existing relative header paths");
        }
    }
    for (const auto& source : sources) {
        if (!path_value_exists(root, source)
            || !extension_in(source, {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"})) {
            add_profile_issue(profile, "error", "invalid-native-component-source",
                              "Native-component sources must be existing relative source paths");
        }
    }

    auto* realtime = object_field(manifest, "realtime");
    if (!realtime || !bool_field(*realtime, "processSafe", false)
        || bool_field(*realtime, "allocatesInProcess", true)
        || bool_field(*realtime, "locksInProcess", true)) {
        add_profile_issue(profile, "error", "native-component-rt-contract",
                          "Native-component realtime block must declare processSafe true with no process allocation or locks");
    }

    if (!profile_ok(profile)) profile.status = "fail";
    return profile;
}

bool manifest_exports_path(const JsonValue& manifest,
                           const std::string& export_key,
                           const std::string& rel_path) {
    if (auto* exports = object_field(manifest, "exports")) {
        const auto paths = string_array_field(*exports, export_key);
        return std::find(paths.begin(), paths.end(), rel_path) != paths.end();
    }
    return false;
}

bool dimensions_match(const JsonValue& profile_dims, const JsonValue& report_dims) {
    return int_field(profile_dims, "width") == int_field(report_dims, "width")
        && int_field(profile_dims, "height") == int_field(report_dims, "height")
        && int_field(profile_dims, "scale") == int_field(report_dims, "scale");
}

#ifdef _WIN32
std::string shell_quote_local(const fs::path& path) {
    const auto s = path.string();
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}
#endif

std::string safe_artifact_stem(std::string value) {
    if (value.empty()) value = "profile";
    for (char& c : value) {
        const auto ch = static_cast<unsigned char>(c);
        if (!std::isalnum(ch) && c != '-' && c != '_') c = '-';
    }
    return value;
}

void add_ui_kit_integration_preview(KitProfileResult& profile,
                                    const JsonValue& manifest,
                                    const std::string& kit_id,
                                    const std::string& entrypoint) {
    if (!profile_ok(profile)) return;
    if (!string_array_contains(manifest, "kind", "ui-kit")) return;
    if (entrypoint.empty()) return;

    auto* exports = object_field(manifest, "exports");
    if (!exports) return;

    const auto scripts = string_array_field(*exports, "pulpUiScripts");
    if (std::find(scripts.begin(), scripts.end(), entrypoint) == scripts.end()) return;

    const auto copied_script = copied_kit_rel_path(kit_id, entrypoint);
    const auto tokens = string_array_field(*exports, "designTokens");
    const auto assets = string_array_field(*exports, "assets");
    const auto kit_target = sanitize_id_for_cmake(kit_id);

    std::string helper = "pulp_use_kit_ui(<plugin-target> " + kit_target
        + " SCRIPT " + copied_script;
    if (tokens.size() == 1) {
        helper += " TOKENS " + copied_kit_rel_path(kit_id, tokens.front());
    } else if (tokens.size() > 1) {
        helper += " TOKENS <reviewed-token-path>";
    }
    helper += ")";

    profile.artifacts.push_back({"ui-kit-cmake-include", "include(cmake/pulp-kits.cmake OPTIONAL)"});
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
        if (fs::exists(candidate)) return candidate;
    }
    auto exe = base;
    exe += ".exe";
    return exe;
#else
    return base;
#endif
}

std::string visual_diff_report_json(const fs::path& expected,
                                    const fs::path& actual,
                                    std::size_t expected_size,
                                    std::size_t actual_size,
                                    std::size_t differing_bytes,
                                    int tolerance_bytes,
                                    bool pass) {
    const auto mode = tolerance_bytes > 0 ? "byte-tolerance" : "exact-bytes";
    return std::string("{\n")
        + "  \"kind\": \"pulp-screenshot-visual-diff\",\n"
        + "  \"status\": " + json_string(pass ? "pass" : "fail") + ",\n"
        + "  \"mode\": " + json_string(mode) + ",\n"
        + "  \"expected\": " + json_string(expected.string()) + ",\n"
        + "  \"actual\": " + json_string(actual.string()) + ",\n"
        + "  \"expected_bytes\": " + std::to_string(expected_size) + ",\n"
        + "  \"actual_bytes\": " + std::to_string(actual_size) + ",\n"
        + "  \"differing_bytes\": " + std::to_string(differing_bytes) + ",\n"
        + "  \"tolerance_bytes\": " + std::to_string(tolerance_bytes) + "\n"
        + "}\n";
}

void maybe_write_visual_diff(KitProfileResult& profile,
                             const fs::path& root,
                             const JsonValue& profile_json,
                             const fs::path& output,
                             const fs::path& out_dir,
                             const std::string& stem) {
    const auto expected_rel = string_field(profile_json, "expectedImage");
    if (expected_rel.empty()) return;

    const int tolerance_bytes = int_field(profile_json, "visualToleranceBytes", 0);
    const auto expected_path = root / fs::path(expected_rel);
    const auto expected = read_bytes(expected_path);
    const auto actual = read_bytes(output);
    const auto common = std::min(expected.size(), actual.size());
    std::size_t differing = expected.size() > actual.size()
        ? expected.size() - actual.size()
        : actual.size() - expected.size();
    for (std::size_t i = 0; i < common; ++i) {
        if (expected[i] != actual[i]) ++differing;
    }

    const bool pass = differing <= static_cast<std::size_t>(std::max(0, tolerance_bytes));
    const auto diff_report = out_dir / (stem + ".visual-diff.json");
    if (write_text(diff_report, visual_diff_report_json(expected_path, output,
                                                        expected.size(), actual.size(),
                                                        differing, tolerance_bytes, pass))) {
        profile.artifacts.push_back({"visual-diff-report", diff_report.string()});
    }
    if (!pass) {
        add_profile_issue(profile, "error", "screenshot-visual-diff-mismatch",
                          "Rendered screenshot does not match expectedImage baseline");
        profile.status = "fail";
    }
}

void maybe_execute_screenshot_profile(KitProfileResult& profile,
                                      const fs::path& root,
                                      const fs::path& project_root,
                                      const JsonValue& profile_json,
                                      const JsonValue& dimensions,
                                      const KitVerifyOptions& options) {
    if (!options.execute_screenshots) return;
    if (!profile_ok(profile)) return;

    const auto entrypoint = string_field(profile_json, "entrypoint");
    const auto screenshot_tool = default_screenshot_tool_for_project(project_root);
    if (!fs::exists(screenshot_tool)) {
        add_profile_issue(profile, "error", "missing-screenshot-tool",
                          "Cannot execute screenshot profile because build/tools/screenshot/pulp-screenshot is missing");
        profile.status = "fail";
        return;
    }

    const auto out_dir = options.screenshot_output_dir.empty()
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
        "--script", (root / fs::path(entrypoint)).string(),
        "--output", output.string(),
        "--width", std::to_string(width),
        "--height", std::to_string(height),
        "--scale", std::to_string(scale),
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

KitProfileResult verify_screenshot_profile(const fs::path& root,
                                           const JsonValue& manifest,
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
        add_profile_issue(profile, "error", "screenshot-entrypoint-not-exported",
                          "Screenshot profile entrypoint must reference an exported `pulpUiScripts` path");
    }

    auto* dimensions = object_field(profile_json, "dimensions");
    if (!dimensions || int_field(*dimensions, "width") <= 0
        || int_field(*dimensions, "height") <= 0 || int_field(*dimensions, "scale") <= 0) {
        add_profile_issue(profile, "error", "invalid-screenshot-dimensions",
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
                add_profile_issue(profile, "error", "screenshot-report-profile-mismatch",
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
    if (has_field(profile_json, "visualToleranceBytes")
        && int_field(profile_json, "visualToleranceBytes", 0) < 0) {
        add_profile_issue(profile, "error", "invalid-screenshot-visual-tolerance",
                          "Screenshot profile visualToleranceBytes must be zero or greater");
    }

    if (dimensions && project_root.empty() && options.execute_screenshots) {
        add_profile_issue(profile, "error", "missing-project-root",
                          "Screenshot execution requires --project or a Pulp project working directory");
    } else if (dimensions) {
        maybe_execute_screenshot_profile(profile, root, project_root, profile_json,
                                         *dimensions, options);
    }

    add_ui_kit_integration_preview(profile, manifest, string_field(manifest, "id"), entrypoint);

    if (!profile_ok(profile)) profile.status = "fail";
    return profile;
}

KitProfileResult verify_generic_profile(const fs::path& profile_rel, const JsonValue& profile_json) {
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
        add_profile_issue(profile, "info", "profile-requires-validation-lane",
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
    if (!validation) return profiles;
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
            profiles.push_back(verify_screenshot_profile(root, manifest, profile_rel,
                                                         profile_json, project_root, options));
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

void print_usage() {
    std::cout
        << "pulp kit — search, validate, inspect, plan, verify, apply, remove, and pack local Pulp package manifests\n\n"
        << "Usage:\n"
        << "  pulp kit search [query] [--root <dir>] [--kind <kind>] [--lane <kit|content>] [--json]\n"
        << "  pulp kit validate <path> [--json] [--strict]\n"
        << "  pulp kit inspect <path> [--json]\n"
        << "  pulp kit plan <path> [--project <dir>] [--json]\n"
        << "  pulp kit verify <path> [--project <dir>] [--json] [--execute-screenshots]\n"
        << "  pulp kit apply <path> [--project <dir>] --yes\n"
        << "  pulp kit remove <kit-id> [--project <dir>] --yes\n"
        << "  pulp kit pack <path> [--output <file>] [--json]\n"
        << "  pulp kit publish <path> --dry-run [--registry-manifest <file>] [--json]\n"
        << "  pulp kit init --kind <source|ui-kit|template> --id <id> [--name <name>] [--dir <path>] [--force]\n\n"
        << "Use kits for reusable Pulp source, UI, templates, validation fixtures, graph nodes, and native components.\n"
        << "Keep curated third-party dependencies on `pulp add <name>`; kits use inspect/plan/approve/apply.\n"
        << "Validation and planning are metadata-only: no CMake, JS, scripts, or dynamic libraries are executed.\n"
        << "Verify runs declared validation-profile checks after plan review. "
        << "--execute-screenshots explicitly runs exported UI scripts through pulp-screenshot.\n";
}

std::string default_name_from_id(const std::string& id) {
    auto pos = id.find_last_of(".:_-");
    auto name = pos == std::string::npos ? id : id.substr(pos + 1);
    if (name.empty()) return "Pulp Kit";
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

std::string init_manifest(const std::string& kind,
                          const std::string& id,
                          const std::string& name) {
    std::string export_block;
    if (kind == "ui-kit") {
        export_block =
            "    \"pulpUiScripts\": [\"ui/index.js\"],\n"
            "    \"designTokens\": [\"ui/tokens.json\"]\n";
    } else if (kind == "template") {
        export_block = "    \"templates\": [\"templates/basic-plugin\"]\n";
    } else {
        export_block = "    \"cmakeTargets\": []\n";
    }

    return std::string("{\n") +
        "  \"schema\": \"pulp-package-v1\",\n"
        "  \"id\": " + json_string(id) + ",\n"
        "  \"name\": " + json_string(name) + ",\n"
        "  \"version\": \"0.1.0\",\n"
        "  \"license\": \"MIT\",\n"
        "  \"licenses\": {\n"
        "    \"code\": \"MIT\"\n"
        "  },\n"
        "  \"kind\": [" + json_string(kind) + "],\n"
        "  \"audience\": [\"developer\", \"agent\"],\n"
        "  \"description\": \"Local developer kit scaffold.\",\n"
        "  \"requires\": {\n"
        "    \"pulp\": \">=0.395.0\",\n"
        "    \"cpp\": 20,\n"
        "    \"platforms\": [\"macOS\", \"Windows\", \"Linux\"]\n"
        "  },\n"
        "  \"capabilities\": [],\n"
        "  \"exports\": {\n" + export_block +
        "  },\n"
        "  \"dependencies\": {\n"
        "    \"pulp\": [],\n"
        "    \"packages\": []\n"
        "  },\n"
        "  \"realtime\": {\n"
        "    \"processSafe\": false,\n"
        "    \"allocatesInProcess\": false,\n"
        "    \"locksInProcess\": false\n"
        "  },\n"
        "  \"agent\": {\n"
        "    \"guidance\": \"AGENTS.md\"\n"
        "  },\n"
        "  \"authoring\": {\n"
        "    \"createdBy\": {\n"
        "      \"type\": \"human\"\n"
        "    },\n"
        "    \"humanReview\": {\n"
        "      \"requiredBeforePublish\": true,\n"
        "      \"reviewed\": true\n"
        "    }\n"
        "  },\n"
        "  \"validation\": {\n"
        "    \"profiles\": []\n"
        "  }\n"
        "}\n";
}

bool valid_init_kind(const std::string& kind) {
    return kind == "source" || kind == "ui-kit" || kind == "template";
}

std::string lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool search_text_matches(const KitValidationResult& result, const std::string& query) {
    if (query.empty() || query == "*") return true;
    const auto q = lower_ascii(query);
    auto contains = [&](std::string_view value) {
        return lower_ascii(value).find(q) != std::string::npos;
    };
    if (contains(result.summary.id) || contains(result.summary.name)
        || contains(result.summary.license)) return true;
    for (const auto& value : result.summary.kinds)
        if (contains(value)) return true;
    for (const auto& value : result.summary.capabilities)
        if (contains(value)) return true;
    for (const auto& value : result.summary.dependency_packages)
        if (contains(value)) return true;
    return false;
}

std::string package_lane(const KitSummary& summary) {
    return vector_contains(summary.kinds, "content-pack") ? "content" : "kit";
}

bool is_content_pack_result(const KitValidationResult& result) {
    return vector_contains(result.summary.kinds, "content-pack");
}

void add_content_pack_kit_lane_issue(KitValidationResult& result,
                                     const std::string& command) {
    add_issue(result, "error", "content-pack-wrong-lane",
              "`pulp kit " + command
                  + "` does not mutate, apply, or publish content packs; use `pulp content validate`, `pulp content preview`, and `pulp content install/update` instead");
}

bool skip_search_dir(const fs::path& path) {
    const auto name = path.filename().string();
    static const std::set<std::string> skip = {
        ".git", ".hg", ".svn", "build", "build-debug", "build-release",
        "node_modules", "external", ".pulp", "prompt-exports"
    };
    return skip.count(name) != 0;
}

std::vector<fs::path> discover_search_inputs(const fs::path& root) {
    std::vector<fs::path> inputs;
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        if (root.filename() == "pulp.package.json" || is_package_archive_path(root))
            inputs.push_back(root);
        return inputs;
    }
    if (!fs::is_directory(root, ec)) return inputs;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && skip_search_dir(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)
            && (it->path().filename() == "pulp.package.json" || is_package_archive_path(it->path())))
            inputs.push_back(it->path());
    }
    std::sort(inputs.begin(), inputs.end());
    return inputs;
}

KitValidationResult validate_search_input(const fs::path& path) {
    if (!is_package_archive_path(path))
        return validate_manifest_path(path, false);

    const auto temp_root = temporary_archive_root();
    std::vector<std::string> archive_issues;
    KitValidationResult result;
    if (validate_and_extract_kit_archive(path, temp_root, archive_issues)) {
        result = validate_manifest_path(temp_root / "pulp.package.json", false);
        result.summary.manifest_path = path;
        result.summary.root = path.parent_path();
    } else {
        result.summary.manifest_path = path;
        result.summary.root = path.parent_path();
        for (const auto& issue : archive_issues)
            add_archive_issue(result, issue);
    }
    std::error_code ec;
    fs::remove_all(temp_root, ec);
    return result;
}

std::string search_result_json(const KitValidationResult& result) {
    return "{\"lane\":" + json_string(package_lane(result.summary))
        + ",\"ok\":" + (result.ok() ? "true" : "false")
        + ",\"kit\":" + summary_json(result.summary)
        + ",\"issues\":" + issues_json(result.issues)
        + "}";
}

int cmd_search(const std::vector<std::string>& args) {
    bool json = false;
    std::string query;
    std::string kind_filter;
    std::string lane_filter;
    fs::path root = fs::current_path();
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                print_fail_local(std::string(flag) + " requires a value");
                return false;
            }
            return true;
        };
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--root") {
            if (!require_value("--root")) return 2;
            root = args[++i];
        } else if (args[i] == "--kind") {
            if (!require_value("--kind")) return 2;
            kind_filter = args[++i];
        } else if (args[i] == "--lane") {
            if (!require_value("--lane")) return 2;
            lane_filter = args[++i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_usage();
            return 0;
        } else if (!args[i].starts_with("-") && query.empty()) {
            query = args[i];
        } else {
            print_fail_local("Unknown kit search argument: " + args[i]);
            return 2;
        }
    }
    if (!lane_filter.empty() && lane_filter != "kit" && lane_filter != "content") {
        print_fail_local("--lane must be `kit` or `content`");
        return 2;
    }
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        print_fail_local("pulp kit search root does not exist: " + root.string());
        return 1;
    }

    std::vector<KitValidationResult> matches;
    for (const auto& input_path : discover_search_inputs(root)) {
        auto result = validate_search_input(input_path);
        if (!kind_filter.empty() && !vector_contains(result.summary.kinds, kind_filter))
            continue;
        if (!lane_filter.empty() && package_lane(result.summary) != lane_filter)
            continue;
        if (!search_text_matches(result, query)) continue;
        matches.push_back(std::move(result));
    }

    if (json) {
        std::cout << "{\"ok\":true"
                  << ",\"root\":" << json_string(root.string())
                  << ",\"query\":" << json_string(query)
                  << ",\"kind\":" << json_string(kind_filter)
                  << ",\"lane\":" << json_string(lane_filter)
                  << ",\"results\":[";
        for (std::size_t i = 0; i < matches.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << search_result_json(matches[i]);
        }
        std::cout << "]}\n";
    } else {
        for (const auto& result : matches) {
            std::cout << package_lane(result.summary) << " "
                      << result.summary.id << " " << result.summary.version
                      << " " << result.summary.manifest_path.string() << "\n";
        }
    }
    return 0;
}

int cmd_validate(const std::vector<std::string>& args) {
    bool json = false;
    bool strict = false;
    std::string path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--strict") {
            strict = true;
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_usage();
            return 0;
        } else if (!args[i].starts_with("-") && path.empty()) {
            path = args[i];
        } else {
            print_fail_local("Unknown kit validate argument: " + args[i]);
            return 2;
        }
    }
    if (path.empty()) {
        print_fail_local("pulp kit validate requires a path");
        return 2;
    }

    KitInputWorkspace input(path);
    auto result = validate_kit_input(input, strict);
    if (json) {
        std::cout << validation_result_json(result) << "\n";
    } else {
        if (result.ok()) {
            print_ok_local("Kit manifest valid: " + result.summary.id);
        } else {
            print_fail_local("Kit manifest invalid: " + result.summary.manifest_path.string());
        }
        for (const auto& issue : result.issues) {
            std::cout << "  [" << issue.severity << "] " << issue.code
                      << ": " << issue.message << "\n";
        }
    }
    return result.ok() ? 0 : 1;
}

int cmd_inspect(const std::vector<std::string>& args) {
    bool json = false;
    std::string path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_usage();
            return 0;
        } else if (!args[i].starts_with("-") && path.empty()) {
            path = args[i];
        } else {
            print_fail_local("Unknown kit inspect argument: " + args[i]);
            return 2;
        }
    }
    if (path.empty()) {
        print_fail_local("pulp kit inspect requires a path");
        return 2;
    }

    KitInputWorkspace input(path);
    auto result = validate_kit_input(input, false);
    if (json) {
        std::cout << validation_result_json(result) << "\n";
    } else {
        std::cout << "Kit: " << result.summary.id << "\n"
                  << "Name: " << result.summary.name << "\n"
                  << "Version: " << result.summary.version << "\n"
                  << "Kind: ";
        for (std::size_t i = 0; i < result.summary.kinds.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << result.summary.kinds[i];
        }
        std::cout << "\nCapabilities: " << result.summary.capabilities.size() << "\n"
                  << "Dependency packages: " << result.summary.dependency_packages.size() << "\n"
                  << "Issues: " << result.issues.size() << "\n";
    }
    return result.ok() ? 0 : 1;
}

int cmd_plan(const std::vector<std::string>& args) {
    bool json = false;
    std::string path;
    fs::path project_root;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                print_fail_local(std::string(flag) + " requires a value");
                return false;
            }
            return true;
        };
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--project") {
            if (!require_value("--project")) return 2;
            project_root = args[++i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_usage();
            return 0;
        } else if (!args[i].starts_with("-") && path.empty()) {
            path = args[i];
        } else {
            print_fail_local("Unknown kit plan argument: " + args[i]);
            return 2;
        }
    }
    if (path.empty()) {
        print_fail_local("pulp kit plan requires a path");
        return 2;
    }
    if (project_root.empty()) project_root = find_project_root_local();
    if (project_root.empty()) {
        print_fail_local("pulp kit plan requires --project or a Pulp project working directory");
        return 1;
    }

    KitInputWorkspace input(path);
    auto result = validate_kit_input(input, true);
    std::vector<KitPlanAction> actions;

    if (result.ok() && is_content_pack_result(result)) {
        add_content_pack_kit_lane_issue(result, "plan");
    }

    if (result.ok()) {
        auto manifest = parse_manifest_json(result.summary.manifest_path);
        const auto manifest_sha256 = "sha256-" + file_sha256(result.summary.manifest_path);
        const auto cmake_targets = object_string_array_field(manifest, "exports", "cmakeTargets");
        const auto ui_scripts = object_string_array_field(manifest, "exports", "pulpUiScripts");
        const auto design_tokens = object_string_array_field(manifest, "exports", "designTokens");
        const auto assets = object_string_array_field(manifest, "exports", "assets");
        const auto templates = object_string_array_field(manifest, "exports", "templates");
        const auto source_files = object_string_array_field(manifest, "exports", "sourceFiles");
        const auto native_headers = object_string_array_field(manifest, "exports", "nativeComponentHeaders");
        const auto native_sources = object_string_array_field(manifest, "exports", "nativeComponentSources");
        const auto node_manifests = object_string_array_field(manifest, "exports", "nodePackManifests");
        const auto graph_fixtures = object_string_array_field(manifest, "exports", "graphFixtures");
        const auto state_fixtures = object_string_array_field(manifest, "exports", "stateFixtures");

        actions.push_back({
            "lock-entry",
            (project_root / ".pulp" / "kits.lock.json").string(),
            "Record kit id, version, source path, manifest digest, declared dependencies, and generated ownership markers.",
            ""
        });
        actions.push_back({
            "manifest-digest",
            result.summary.manifest_path.string(),
            "Pin exact reviewed pulp.package.json bytes as " + manifest_sha256 + ".",
            ""
        });

        actions.push_back({
            "generated-cmake",
            (project_root / "cmake" / "pulp-kits.cmake").string(),
            "Add generated CMake scaffolding with package ownership markers and kit interface targets.",
            ""
        });

        if (!ui_scripts.empty() || !design_tokens.empty() || !assets.empty()) {
            actions.push_back({
                "ui-kit-interface",
                sanitize_id_for_cmake(result.summary.id),
                "Expose copied UI scripts, design tokens, and assets as CMake target properties.",
                ""
            });
        }

        for (const auto& dep : result.summary.dependency_packages) {
            actions.push_back({
                "dependency-package",
                dep,
                "Resolve curated dependency package through the existing pulp add registry path.",
                "pulp add " + dep
            });
        }

        for (const auto& rel : ui_scripts) {
            actions.push_back({
                "copy-ui-script",
                (project_root / "pulp-kits" / result.summary.id / rel).string(),
                "Copy or embed declared Pulp UI script after explicit apply approval.",
                ""
            });
        }
        for (const auto& rel : design_tokens) {
            actions.push_back({
                "copy-design-token",
                (project_root / "pulp-kits" / result.summary.id / rel).string(),
                "Copy or embed declared design token file after explicit apply approval.",
                ""
            });
        }
        for (const auto& rel : assets) {
            actions.push_back({
                "copy-asset",
                (project_root / "pulp-kits" / result.summary.id / rel).string(),
                "Copy declared asset path after explicit apply approval.",
                ""
            });
        }
        for (const auto& rel : source_files) {
            actions.push_back({
                "copy-source-file",
                (project_root / "pulp-kits" / result.summary.id / rel).string(),
                "Copy declared source file for explicit project integration after approval.",
                ""
            });
        }
        for (const auto& rel : native_headers) {
            actions.push_back({
                "copy-native-component-header",
                (project_root / "pulp-kits" / result.summary.id / rel).string(),
                "Copy declared native component header after approval.",
                ""
            });
        }
        for (const auto& rel : native_sources) {
            actions.push_back({
                "copy-native-component-source",
                (project_root / "pulp-kits" / result.summary.id / rel).string(),
                "Copy declared native component source after approval.",
                ""
            });
        }
        for (const auto& rel : node_manifests) {
            actions.push_back({
                "copy-node-pack-manifest",
                (project_root / "pulp-kits" / result.summary.id / rel).string(),
                "Copy signed node-pack manifest metadata without loading dynamic code.",
                ""
            });
        }
        for (const auto& rel : graph_fixtures) {
            actions.push_back({
                "copy-graph-fixture",
                (project_root / "pulp-kits" / result.summary.id / rel).string(),
                "Copy SignalGraph validation fixture after approval.",
                ""
            });
        }
        for (const auto& rel : state_fixtures) {
            actions.push_back({
                "copy-state-fixture",
                (project_root / "pulp-kits" / result.summary.id / rel).string(),
                "Copy state validation fixture after approval.",
                ""
            });
        }
        for (const auto& rel : templates) {
            actions.push_back({
                "template-source",
                (result.summary.root / rel).string(),
                "Make template files available to a future package-backed create workflow; no files are generated by plan.",
                ""
            });
        }

        const auto registry_path = project_root / "tools" / "packages" / "registry.json";
        if (!result.summary.dependency_packages.empty()) {
            auto registry_result = pulp::cli::pkg::load_registry(registry_path);
            if (!registry_result.error.empty()) {
                add_issue(result, "error", "missing-registry",
                          "Cannot resolve kit dependency packages: " + registry_result.error);
            } else {
                for (const auto& dep : result.summary.dependency_packages) {
                    if (registry_result.registry.packages.find(dep)
                        == registry_result.registry.packages.end()) {
                        add_issue(result, "error", "unknown-dependency-package",
                                  "Kit dependency package `" + dep
                                      + "` is not present in the curated registry");
                    }
                }
            }
        }
    }

    const auto ok = result.ok();
    if (json) {
        std::cout << "{\"ok\":" << (ok ? "true" : "false")
                  << ",\"project_root\":" << json_string(project_root.string())
                  << ",\"kit\":" << summary_json(result.summary)
                  << ",\"actions\":" << actions_json(actions)
                  << ",\"issues\":" << issues_json(result.issues)
                  << "}\n";
    } else {
        if (ok) {
            print_ok_local("Kit plan ready: " + result.summary.id);
        } else {
            print_fail_local("Kit plan failed: " + result.summary.manifest_path.string());
        }
        std::cout << "Project: " << project_root << "\n";
        for (const auto& action : actions) {
            std::cout << "  - " << action.kind << ": " << action.description << "\n";
            if (!action.path.empty()) std::cout << "    " << action.path << "\n";
            if (!action.command.empty()) std::cout << "    " << action.command << "\n";
        }
        for (const auto& issue : result.issues) {
            std::cout << "  [" << issue.severity << "] " << issue.code
                      << ": " << issue.message << "\n";
        }
    }
    return ok ? 0 : 1;
}

int cmd_verify(const std::vector<std::string>& args) {
    bool json = false;
    std::string path;
    fs::path project_root;
    KitVerifyOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                print_fail_local(std::string(flag) + " requires a value");
                return false;
            }
            return true;
        };
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--execute-screenshots") {
            options.execute_screenshots = true;
        } else if (args[i] == "--screenshot-backend") {
            if (!require_value("--screenshot-backend")) return 2;
            options.screenshot_backend = args[++i];
        } else if (args[i] == "--screenshot-output-dir") {
            if (!require_value("--screenshot-output-dir")) return 2;
            options.screenshot_output_dir = args[++i];
        } else if (args[i] == "--project") {
            if (!require_value("--project")) return 2;
            project_root = args[++i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_usage();
            return 0;
        } else if (!args[i].starts_with("-") && path.empty()) {
            path = args[i];
        } else {
            print_fail_local("Unknown kit verify argument: " + args[i]);
            return 2;
        }
    }
    if (path.empty()) {
        print_fail_local("pulp kit verify requires a path");
        return 2;
    }
    if (project_root.empty()) project_root = find_project_root_local();
    if (options.execute_screenshots && project_root.empty()) {
        print_fail_local("pulp kit verify --execute-screenshots requires --project or a Pulp project working directory");
        return 2;
    }

    KitInputWorkspace input(path);
    auto result = validate_kit_input(input, true);
    std::vector<KitProfileResult> profiles;
    if (result.ok()) {
        auto manifest = parse_manifest_json(result.summary.manifest_path);
        profiles = verify_profiles(result.summary.manifest_path, manifest, project_root, options);
        if (profiles.empty()) {
            add_issue(result, "warning", "no-validation-profiles",
                      "Kit declares no validation profiles to verify");
        }
        for (const auto& profile : profiles) {
            if (!profile_ok(profile)) {
                add_issue(result, "error", "validation-profile-failed",
                          "Validation profile failed: " + profile.path);
            }
        }
    }

    const auto ok = result.ok();
    if (json) {
        std::cout << "{\"ok\":" << (ok ? "true" : "false")
                  << ",\"project_root\":" << json_string(project_root.string())
                  << ",\"kit\":" << summary_json(result.summary)
                  << ",\"profiles\":" << profile_results_json(profiles)
                  << ",\"issues\":" << issues_json(result.issues)
                  << "}\n";
    } else {
        if (ok) {
            print_ok_local("Kit validation profiles verified: " + result.summary.id);
        } else {
            print_fail_local("Kit validation profile verification failed: "
                             + result.summary.manifest_path.string());
        }
        if (!project_root.empty()) std::cout << "Project: " << project_root << "\n";
        for (const auto& profile : profiles) {
            std::cout << "  - " << profile.status << ": " << profile.path;
            if (!profile.kind.empty()) std::cout << " (" << profile.kind << ")";
            std::cout << "\n";
            for (const auto& issue : profile.issues) {
                std::cout << "    [" << issue.severity << "] " << issue.code
                          << ": " << issue.message << "\n";
            }
        }
        for (const auto& issue : result.issues) {
            std::cout << "  [" << issue.severity << "] " << issue.code
                      << ": " << issue.message << "\n";
        }
    }
    return ok ? 0 : 1;
}

int cmd_apply(const std::vector<std::string>& args) {
    bool yes = false;
    std::string path;
    fs::path project_root;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                print_fail_local(std::string(flag) + " requires a value");
                return false;
            }
            return true;
        };
        if (args[i] == "--yes") {
            yes = true;
        } else if (args[i] == "--project") {
            if (!require_value("--project")) return 2;
            project_root = args[++i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_usage();
            return 0;
        } else if (!args[i].starts_with("-") && path.empty()) {
            path = args[i];
        } else {
            print_fail_local("Unknown kit apply argument: " + args[i]);
            return 2;
        }
    }
    if (!yes) {
        print_fail_local("pulp kit apply requires --yes after reviewing `pulp kit plan`");
        return 2;
    }
    if (path.empty()) {
        print_fail_local("pulp kit apply requires a path");
        return 2;
    }
    if (project_root.empty()) project_root = find_project_root_local();
    if (project_root.empty()) {
        print_fail_local("pulp kit apply requires --project or a Pulp project working directory");
        return 1;
    }
    if (!fs::exists(project_root / "CMakeLists.txt")) {
        print_fail_local("pulp kit apply requires a project CMakeLists.txt before writing kit files");
        return 1;
    }

    KitInputWorkspace input(path);
    auto result = validate_kit_input(input, true);
    if (result.ok() && is_content_pack_result(result)) {
        add_content_pack_kit_lane_issue(result, "apply");
    }
    if (!result.ok()) {
        print_fail_local("Kit manifest invalid; run `pulp kit validate --strict`");
        for (const auto& issue : result.issues) {
            std::cout << "  [" << issue.severity << "] " << issue.code
                      << ": " << issue.message << "\n";
        }
        return 1;
    }

    const auto lock_path = project_root / ".pulp" / "kits.lock.json";
    const auto generated_cmake_path = project_root / "cmake" / "pulp-kits.cmake";
    const auto cmakelists_path = project_root / "CMakeLists.txt";
    const auto old_lock = snapshot_file(lock_path);
    const auto old_generated_cmake = snapshot_file(generated_cmake_path);
    const auto old_cmakelists = snapshot_file(cmakelists_path);
    auto entries = load_kit_lock(lock_path);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const KitLockEntry& entry) {
        return entry.id == result.summary.id;
    }), entries.end());

    auto manifest = parse_manifest_json(result.summary.manifest_path);
    const auto manifest_sha256 = "sha256-" + file_sha256(result.summary.manifest_path);
    const auto cmake_targets = object_string_array_field(manifest, "exports", "cmakeTargets");
    const auto ui_scripts = object_string_array_field(manifest, "exports", "pulpUiScripts");
    const auto design_tokens = object_string_array_field(manifest, "exports", "designTokens");
    const auto assets = object_string_array_field(manifest, "exports", "assets");
    const auto templates = object_string_array_field(manifest, "exports", "templates");
    const auto source_files = object_string_array_field(manifest, "exports", "sourceFiles");
    const auto native_headers = object_string_array_field(manifest, "exports", "nativeComponentHeaders");
    const auto native_sources = object_string_array_field(manifest, "exports", "nativeComponentSources");
    const auto node_manifests = object_string_array_field(manifest, "exports", "nodePackManifests");
    const auto graph_fixtures = object_string_array_field(manifest, "exports", "graphFixtures");
    const auto state_fixtures = object_string_array_field(manifest, "exports", "stateFixtures");

    if (!result.summary.dependency_packages.empty()) {
        const auto registry_path = project_root / "tools" / "packages" / "registry.json";
        auto registry_result = pulp::cli::pkg::load_registry(registry_path);
        if (!registry_result.error.empty()) {
            print_fail_local("Cannot resolve kit dependency packages: " + registry_result.error);
            return 1;
        }
        auto installed = pulp::cli::pkg::load_lock_file(project_root / "packages.lock.json");
        for (const auto& dep : result.summary.dependency_packages) {
            if (registry_result.registry.packages.find(dep)
                == registry_result.registry.packages.end()) {
                print_fail_local("Dependency package `" + dep
                                 + "` is not present in the curated registry");
                return 1;
            }
            if (installed.packages.find(dep) == installed.packages.end()) {
                print_fail_local("Dependency package `" + dep
                                 + "` is not installed; run `pulp add " + dep
                                 + "` and review the plan again");
                return 1;
            }
        }
    }

    KitLockEntry entry;
    entry.id = result.summary.id;
    entry.version = result.summary.version;
    entry.source = kit_source_label(input, result);
    entry.manifest_sha256 = manifest_sha256;
    entry.kinds = result.summary.kinds;
    entry.dependency_packages = result.summary.dependency_packages;
    entry.cmake_targets = cmake_targets;
    for (const auto& rel : ui_scripts)
        entry.ui_scripts.push_back(copied_kit_rel_path(result.summary.id, rel));
    for (const auto& rel : design_tokens)
        entry.design_tokens.push_back(copied_kit_rel_path(result.summary.id, rel));
    for (const auto& rel : assets)
        entry.assets.push_back(copied_kit_rel_path(result.summary.id, rel));
    for (const auto& rel : source_files)
        entry.source_files.push_back(copied_kit_rel_path(result.summary.id, rel));
    for (const auto& rel : native_headers)
        entry.native_component_headers.push_back(copied_kit_rel_path(result.summary.id, rel));
    for (const auto& rel : native_sources)
        entry.native_component_sources.push_back(copied_kit_rel_path(result.summary.id, rel));
    for (const auto& rel : node_manifests)
        entry.node_pack_manifests.push_back(copied_kit_rel_path(result.summary.id, rel));
    for (const auto& rel : graph_fixtures)
        entry.graph_fixtures.push_back(copied_kit_rel_path(result.summary.id, rel));
    for (const auto& rel : state_fixtures)
        entry.state_fixtures.push_back(copied_kit_rel_path(result.summary.id, rel));
    entry.owned_paths.push_back(".pulp/kits.lock.json");

    entry.owned_paths.push_back("cmake/pulp-kits.cmake");

    const auto kit_install_root = project_root / "pulp-kits" / result.summary.id;
    const auto kit_backup_root = project_root / "pulp-kits" /
        (".pulp-kit-apply-backup-" + result.summary.id + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool moved_existing_kit_root = false;
    std::error_code replace_ec;
    if (fs::exists(kit_install_root, replace_ec)) {
        fs::remove_all(kit_backup_root, replace_ec);
        if (replace_ec) {
            print_fail_local("Failed to clear kit apply backup: " + replace_ec.message());
            return 1;
        }
        fs::rename(kit_install_root, kit_backup_root, replace_ec);
        if (replace_ec) {
            print_fail_local("Failed to back up existing kit before apply: " + replace_ec.message());
            return 1;
        }
        moved_existing_kit_root = true;
    }

    std::string copy_error;
    std::vector<std::string> rollback_paths;
    auto rollback_after_failure = [&]() {
        rollback_apply_files(project_root, rollback_paths);
        std::error_code rollback_ec;
        fs::remove_all(kit_install_root, rollback_ec);
        if (moved_existing_kit_root) {
            fs::rename(kit_backup_root, kit_install_root, rollback_ec);
        }
        restore_file_snapshot(lock_path, old_lock);
        restore_file_snapshot(generated_cmake_path, old_generated_cmake);
        restore_file_snapshot(cmakelists_path, old_cmakelists);
    };
    for (const auto& rel : ui_scripts) {
        if (!copy_declared_path(result.summary.root, project_root, result.summary.id,
                                rel, entry.owned_paths, &rollback_paths, copy_error)) {
            rollback_after_failure();
            print_fail_local(copy_error);
            return 1;
        }
    }
    for (const auto& rel : design_tokens) {
        if (!copy_declared_path(result.summary.root, project_root, result.summary.id,
                                rel, entry.owned_paths, &rollback_paths, copy_error)) {
            rollback_after_failure();
            print_fail_local(copy_error);
            return 1;
        }
    }
    for (const auto& rel : assets) {
        if (!copy_declared_path(result.summary.root, project_root, result.summary.id,
                                rel, entry.owned_paths, &rollback_paths, copy_error)) {
            rollback_after_failure();
            print_fail_local(copy_error);
            return 1;
        }
    }
    for (const auto& rel : templates) {
        if (!copy_declared_path(result.summary.root, project_root, result.summary.id,
                                rel, entry.owned_paths, &rollback_paths, copy_error)) {
            rollback_after_failure();
            print_fail_local(copy_error);
            return 1;
        }
    }
    for (const auto& rel : source_files) {
        if (!copy_declared_path(result.summary.root, project_root, result.summary.id,
                                rel, entry.owned_paths, &rollback_paths, copy_error)) {
            rollback_after_failure();
            print_fail_local(copy_error);
            return 1;
        }
    }
    for (const auto& rel : native_headers) {
        if (!copy_declared_path(result.summary.root, project_root, result.summary.id,
                                rel, entry.owned_paths, &rollback_paths, copy_error)) {
            rollback_after_failure();
            print_fail_local(copy_error);
            return 1;
        }
    }
    for (const auto& rel : native_sources) {
        if (!copy_declared_path(result.summary.root, project_root, result.summary.id,
                                rel, entry.owned_paths, &rollback_paths, copy_error)) {
            rollback_after_failure();
            print_fail_local(copy_error);
            return 1;
        }
    }
    for (const auto& rel : node_manifests) {
        if (!copy_declared_path(result.summary.root, project_root, result.summary.id,
                                rel, entry.owned_paths, &rollback_paths, copy_error)) {
            rollback_after_failure();
            print_fail_local(copy_error);
            return 1;
        }
    }
    for (const auto& rel : graph_fixtures) {
        if (!copy_declared_path(result.summary.root, project_root, result.summary.id,
                                rel, entry.owned_paths, &rollback_paths, copy_error)) {
            rollback_after_failure();
            print_fail_local(copy_error);
            return 1;
        }
    }
    for (const auto& rel : state_fixtures) {
        if (!copy_declared_path(result.summary.root, project_root, result.summary.id,
                                rel, entry.owned_paths, &rollback_paths, copy_error)) {
            rollback_after_failure();
            print_fail_local(copy_error);
            return 1;
        }
    }

    entries.push_back(std::move(entry));
    if (!write_text(lock_path, kit_lock_json(entries))) {
        rollback_after_failure();
        print_fail_local("Failed to write " + lock_path.string());
        return 1;
    }
    if (!write_text(generated_cmake_path, generated_cmake(entries))) {
        rollback_after_failure();
        print_fail_local("Failed to write " + generated_cmake_path.string());
        return 1;
    }
    if (!ensure_cmake_include(project_root)) {
        rollback_after_failure();
        print_fail_local("Failed to add cmake/pulp-kits.cmake include to CMakeLists.txt");
        return 1;
    }
    if (moved_existing_kit_root) {
        fs::remove_all(kit_backup_root, replace_ec);
        if (replace_ec) {
            print_fail_local("Failed to remove kit apply backup: " + replace_ec.message());
            return 1;
        }
    }

    print_ok_local("Applied kit " + result.summary.id);
    std::cout << "Lock: " << lock_path << "\n";
    std::cout << "CMake: " << generated_cmake_path << "\n";
    return 0;
}

int cmd_remove(const std::vector<std::string>& args) {
    bool yes = false;
    std::string kit_id;
    fs::path project_root;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                print_fail_local(std::string(flag) + " requires a value");
                return false;
            }
            return true;
        };
        if (args[i] == "--yes") {
            yes = true;
        } else if (args[i] == "--project") {
            if (!require_value("--project")) return 2;
            project_root = args[++i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_usage();
            return 0;
        } else if (!args[i].starts_with("-") && kit_id.empty()) {
            kit_id = args[i];
        } else {
            print_fail_local("Unknown kit remove argument: " + args[i]);
            return 2;
        }
    }
    if (!yes) {
        print_fail_local("pulp kit remove requires --yes after reviewing installed kit ownership");
        return 2;
    }
    if (kit_id.empty()) {
        print_fail_local("pulp kit remove requires a kit id");
        return 2;
    }
    if (project_root.empty()) project_root = find_project_root_local();
    if (project_root.empty()) {
        print_fail_local("pulp kit remove requires --project or a Pulp project working directory");
        return 1;
    }

    const auto lock_path = project_root / ".pulp" / "kits.lock.json";
    auto entries = load_kit_lock(lock_path);
    auto it = std::find_if(entries.begin(), entries.end(), [&](const KitLockEntry& entry) {
        return entry.id == kit_id;
    });
    if (it == entries.end()) {
        print_fail_local("Kit `" + kit_id + "` is not installed in " + lock_path.string());
        return 1;
    }

    std::string remove_error;
    for (const auto& rel : it->owned_paths) {
        if (!remove_owned_path(project_root, rel, remove_error)) {
            print_fail_local(remove_error);
            return 1;
        }
        auto parent = fs::path(rel).parent_path();
        if (!parent.empty()) prune_empty_parent_dirs(project_root, parent);
    }

    const std::size_t removed_count = it->owned_paths.size(); entries.erase(it);
    if (!entries.empty()) {
        if (!write_text(lock_path, kit_lock_json(entries))) {
            print_fail_local("Failed to update " + lock_path.string());
            return 1;
        }
        if (!write_text(project_root / "cmake" / "pulp-kits.cmake", generated_cmake(entries))) {
            print_fail_local("Failed to regenerate " + (project_root / "cmake" / "pulp-kits.cmake").string());
            return 1;
        }
    } else {
        std::error_code ec;
        fs::remove(lock_path, ec);
        fs::remove(project_root / "cmake" / "pulp-kits.cmake", ec);
        prune_empty_parent_dirs(project_root, ".pulp");
        prune_empty_parent_dirs(project_root, "cmake");
        prune_empty_parent_dirs(project_root, "pulp-kits");
        if (!remove_cmake_include_if_unused(project_root)) {
            print_fail_local("Failed to remove cmake/pulp-kits.cmake include from CMakeLists.txt");
            return 1;
        }
    }

    print_ok_local("Removed kit " + kit_id + " (removed " + std::to_string(removed_count) + (removed_count == 1 ? " file)" : " files)"));
    return 0;
}

int cmd_pack(const std::vector<std::string>& args) {
    bool json = false;
    std::string path;
    fs::path output;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                print_fail_local(std::string(flag) + " requires a value");
                return false;
            }
            return true;
        };
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--output" || args[i] == "-o") {
            if (!require_value("--output")) return 2;
            output = args[++i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_usage();
            return 0;
        } else if (!args[i].starts_with("-") && path.empty()) {
            path = args[i];
        } else {
            print_fail_local("Unknown kit pack argument: " + args[i]);
            return 2;
        }
    }
    if (path.empty()) {
        print_fail_local("pulp kit pack requires a path");
        return 2;
    }

    auto result = validate_manifest_path(path, true);
    if (!result.ok()) {
        if (json) {
            std::cout << "{\"ok\":false,\"kit\":" << summary_json(result.summary)
                      << ",\"issues\":" << issues_json(result.issues) << "}\n";
        } else {
            print_fail_local("Kit manifest invalid; run `pulp kit validate --strict`");
            for (const auto& issue : result.issues) {
                std::cout << "  [" << issue.severity << "] " << issue.code
                          << ": " << issue.message << "\n";
            }
        }
        return 1;
    }
    if (output.empty()) output = default_pack_output(result.summary);

    std::string error;
    std::vector<fs::path> files;
    if (!collect_pack_files(result.summary.root, files, error)) {
        if (json) {
            std::cout << "{\"ok\":false,\"output\":" << json_string(output.string())
                      << ",\"error\":" << json_string(error) << "}\n";
        } else {
            print_fail_local(error);
        }
        return 1;
    }
    const auto sha_manifest = sha_manifest_json(result.summary.root, files);
    if (!write_zip_archive(result.summary.root, output, files, sha_manifest, error)) {
        if (json) {
            std::cout << "{\"ok\":false,\"output\":" << json_string(output.string())
                      << ",\"error\":" << json_string(error) << "}\n";
        } else {
            print_fail_local(error);
        }
        return 1;
    }

    if (json) {
        std::cout << "{\"ok\":true"
                  << ",\"output\":" << json_string(output.string())
                  << ",\"kit\":" << summary_json(result.summary)
                  << ",\"file_count\":" << (files.size() + 1)
                  << ",\"sha_manifest\":\"files.sha256.json\""
                  << "}\n";
    } else {
        print_ok_local("Packed " + output.string());
        std::cout << "Files: " << (files.size() + 1) << "\n";
        std::cout << "Digest manifest: files.sha256.json\n";
    }
    return 0;
}

int cmd_publish(const std::vector<std::string>& args) {
    bool json = false;
    bool dry_run = false;
    std::string path;
    fs::path registry_manifest_path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                print_fail_local(std::string(flag) + " requires a value");
                return false;
            }
            return true;
        };
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--dry-run") {
            dry_run = true;
        } else if (args[i] == "--registry-manifest") {
            if (!require_value("--registry-manifest")) return 2;
            registry_manifest_path = args[++i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_usage();
            return 0;
        } else if (!args[i].starts_with("-") && path.empty()) {
            path = args[i];
        } else {
            print_fail_local("Unknown kit publish argument: " + args[i]);
            return 2;
        }
    }
    if (path.empty()) {
        print_fail_local("pulp kit publish requires a path");
        return 2;
    }
    if (!dry_run) {
        print_fail_local("pulp kit publish only supports --dry-run until signed registry policy exists");
        return 2;
    }

    KitInputWorkspace input(path);
    auto result = validate_kit_input(input, true);
    JsonValue manifest;
    if (fs::exists(result.summary.manifest_path)) {
        manifest = parse_manifest_json(result.summary.manifest_path);
        if (is_content_pack_result(result)) {
            add_content_pack_kit_lane_issue(result, "publish");
        }
        validate_publish_policy(result, manifest);
        if (!registry_manifest_path.empty()) {
            if (!fs::exists(registry_manifest_path)) {
                add_publish_issue(result, "error", "missing-registry-manifest",
                                  "registry manifest path does not exist: "
                                      + registry_manifest_path.string());
            } else {
                auto registry_manifest = parse_manifest_json(registry_manifest_path);
                validate_registry_manifest(result, registry_manifest,
                                           "sha256-" + file_sha256(result.summary.manifest_path));
            }
        }
    }

    const auto ok = result.ok();
    const auto badges = publish_badges_json(result, !registry_manifest_path.empty());
    const auto compatibility = manifest.type == JsonValue::Object
        ? compatibility_json(manifest)
        : std::string{"{}"};
    if (json) {
        std::cout << "{\"ok\":" << (ok ? "true" : "false")
                  << ",\"dry_run\":true"
                  << ",\"publishing_enabled\":false"
                  << ",\"kit\":" << summary_json(result.summary)
                  << ",\"registry_manifest\":" << json_string(registry_manifest_path.string())
                  << ",\"checks\":[\"strict-manifest\",\"license-inventory\",\"notice-compatibility\",\"human-review\",\"validation-profiles\",\"kind-specific-evidence\",\"signed-canonical-manifest\"]"
                  << ",\"quality_badges\":" << badges
                  << ",\"compatibility\":" << compatibility
                  << ",\"issues\":" << issues_json(result.issues)
                  << "}\n";
    } else {
        if (ok) {
            print_ok_local("Publish dry-run passed: " + result.summary.id);
        } else {
            print_fail_local("Publish dry-run failed: " + result.summary.manifest_path.string());
        }
        std::cout << "Remote publishing: disabled until signed registry policy exists\n";
        if (!registry_manifest_path.empty())
            std::cout << "Registry manifest: " << registry_manifest_path.string() << "\n";
        std::cout << "Quality badges: " << badges << "\n";
        std::cout << "Compatibility: " << compatibility << "\n";
        for (const auto& issue : result.issues) {
            std::cout << "  [" << issue.severity << "] " << issue.code
                      << ": " << issue.message << "\n";
        }
    }
    return ok ? 0 : 1;
}

int cmd_init(const std::vector<std::string>& args) {
    std::string kind;
    std::string id;
    std::string name;
    fs::path dir = fs::current_path();
    bool force = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                print_fail_local(std::string(flag) + " requires a value");
                return false;
            }
            return true;
        };
        if (args[i] == "--kind") {
            if (!require_value("--kind")) return 2;
            kind = args[++i];
        } else if (args[i] == "--id") {
            if (!require_value("--id")) return 2;
            id = args[++i];
        } else if (args[i] == "--name") {
            if (!require_value("--name")) return 2;
            name = args[++i];
        } else if (args[i] == "--dir") {
            if (!require_value("--dir")) return 2;
            dir = args[++i];
        } else if (args[i] == "--force") {
            force = true;
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_usage();
            return 0;
        } else {
            print_fail_local("Unknown kit init argument: " + args[i]);
            return 2;
        }
    }

    if (!valid_init_kind(kind)) {
        print_fail_local("pulp kit init requires --kind source, ui-kit, or template");
        return 2;
    }
    if (!valid_package_id(id)) {
        print_fail_local("pulp kit init requires --id with a valid package id");
        return 2;
    }
    if (name.empty()) name = default_name_from_id(id);

    auto manifest = dir / "pulp.package.json";
    if (fs::exists(manifest) && !force) {
        print_fail_local("pulp.package.json already exists; pass --force to overwrite");
        return 1;
    }
    if (!write_text(manifest, init_manifest(kind, id, name))) {
        print_fail_local("Failed to write " + manifest.string());
        return 1;
    }
    if (!fs::exists(dir / "AGENTS.md")) {
        (void)write_text(dir / "AGENTS.md",
                         "# Kit Guidance\n\nThis file is advisory. Review plans before applying changes.\n");
    }
    print_ok_local("Created " + manifest.string());
    return 0;
}

}  // namespace

bool KitValidationResult::ok() const {
    return std::none_of(issues.begin(), issues.end(), [](const KitIssue& issue) {
        return issue.severity == "error";
    });
}

KitValidationResult validate_manifest_path(const fs::path& input, bool strict) {
    KitValidationResult result;
    result.summary.manifest_path = resolve_manifest_path(input);
    result.summary.root = manifest_root_for(result.summary.manifest_path);

    if (result.summary.manifest_path.empty() || !fs::exists(result.summary.manifest_path)) {
        add_issue(result, "error", "missing-manifest",
                  "No pulp.package.json found at " + result.summary.manifest_path.string());
        return result;
    }

    const auto text = read_text(result.summary.manifest_path);
    if (text.empty()) {
        add_issue(result, "error", "empty-manifest",
                  "Manifest is empty or unreadable: " + result.summary.manifest_path.string());
        return result;
    }

    JsonParser parser{text};
    auto root = parser.parse();
    if (root.type != JsonValue::Object) {
        add_issue(result, "error", "invalid-json", "Manifest root must be a JSON object");
        return result;
    }

    validate_required_string(result, root, "schema");
    validate_required_string(result, root, "id");
    validate_required_string(result, root, "name");
    validate_required_string(result, root, "version");
    validate_required_string(result, root, "license");
    validate_required_array(result, root, "kind");
    validate_required_array(result, root, "capabilities");
    validate_required_object(result, root, "exports");
    validate_required_object(result, root, "dependencies");
    validate_required_object(result, root, "validation");
    validate_manifest_array_shape(result, root);

    result.summary.schema = string_field(root, "schema");
    result.summary.id = string_field(root, "id");
    result.summary.name = string_field(root, "name");
    result.summary.version = string_field(root, "version");
    result.summary.license = string_field(root, "license");
    result.summary.kinds = string_array_field(root, "kind");
    result.summary.capabilities = string_array_field(root, "capabilities");
    if (auto* deps = object_field(root, "dependencies"))
        result.summary.dependency_packages = string_array_field(*deps, "packages");

    if (result.summary.schema != "pulp-package-v1") {
        add_issue(result, "error", "unsupported-schema",
                  "`schema` must be `pulp-package-v1`");
    }
    if (!valid_package_id(result.summary.id)) {
        add_issue(result, "error", "invalid-id",
                  "`id` must contain only letters, numbers, '.', '-', '_', or ':'");
    } else if (!valid_package_path_component(result.summary.id)) {
        add_issue(result, "error", "invalid-id",
                  "`id` must be safe as a cross-platform project path component; avoid ':' or dot-only names");
    }
    if (!valid_semverish(result.summary.version)) {
        add_issue(result, "error", "invalid-version",
                  "`version` must be a semver-like string");
    }
    if (result.summary.kinds.empty()) {
        add_issue(result, "error", "missing-kind", "`kind` must declare at least one package kind");
    }

    validate_license_inventory(result, root, strict);
    validate_realtime(result, root, result.summary.kinds);
    validate_pulp_sdk_requirement(result, root);
    validate_cpp_requirement(result, root);
    validate_pulp_module_dependencies(result, root);
    validate_platforms(result, root, result.summary.kinds);
    validate_declared_paths(result, result.summary.root, root);
    validate_agent_authoring(result, root);

    if (strict && !has_field(root, "authoring")) {
        add_issue(result, "error", "missing-authoring",
                  "`authoring` block is required in strict mode");
    }

    return result;
}

std::string validation_result_json(const KitValidationResult& result) {
    return "{\"ok\":" + std::string(result.ok() ? "true" : "false")
        + ",\"summary\":" + summary_json(result.summary)
        + ",\"issues\":" + issues_json(result.issues) + "}";
}

int cmd_kit(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_usage();
        return 0;
    }
    std::vector<std::string> tail(args.begin() + 1, args.end());
    if (args[0] == "search") return cmd_search(tail);
    if (args[0] == "validate") return cmd_validate(tail);
    if (args[0] == "inspect" || args[0] == "show") return cmd_inspect(tail);
    if (args[0] == "plan") return cmd_plan(tail);
    if (args[0] == "verify") return cmd_verify(tail);
    if (args[0] == "apply") return cmd_apply(tail);
    if (args[0] == "remove" || args[0] == "uninstall") return cmd_remove(tail);
    if (args[0] == "pack") return cmd_pack(tail);
    if (args[0] == "publish") return cmd_publish(tail);
    if (args[0] == "init") return cmd_init(tail);

    print_fail_local("Unknown kit subcommand: " + args[0]);
    print_usage();
    return 2;
}

}  // namespace pulp::cli::kit
