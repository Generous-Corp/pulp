// SPDX-License-Identifier: MIT
//
// kit_manifest_validation.cpp — `pulp.package.json` manifest validation.
// Extracted from kit_commands.cpp (see tools/cli/KIT_COMMANDS_MODULE_MAP.md,
// step 1). This layer is side-effect-free except for reading the manifest and
// the local files it must exist-check / hash. It performs no package execution:
// no CMake, JS, scripts, dylibs, or tool hooks run while validating.

#include "kit_manifest_validation.hpp"

#include "package_registry.hpp"
#include "pulp_version_gen.h"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::cli::kit {

namespace {

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

bool valid_authoring_creator_type(const std::string& type) {
    return type.empty() || type == "human" || type == "agent" || type == "mixed";
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

}  // namespace pulp::cli::kit
