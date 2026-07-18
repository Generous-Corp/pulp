// SPDX-License-Identifier: MIT
#pragma once

// Manifest-validation module for `pulp.package.json`. Owns required-field,
// schema/kind/capability, dependency shape, declared-path, and evidence-hash
// checks plus the public `validate_manifest_path` result contract. See
// tools/cli/KIT_COMMANDS_MODULE_MAP.md for the boundary.
//
// The small manifest-domain helpers below stay header-inline because they are
// shared with the rest of kit_commands.cpp (publish policy, apply, init) — not
// only the validators. The validator bodies and their private helpers live in
// kit_manifest_validation.cpp.

#include "cli_fs_util.hpp"
#include "kit_commands.hpp"
#include "kit_json_helpers.hpp"

#include <cctype>
#include <string>
#include <system_error>

namespace pulp::cli::kit {

// Append a validation issue to the result set. Shared: policy/apply/archive
// code in kit_commands.cpp also records issues through this accumulator.
inline void add_issue(KitValidationResult& result,
                      std::string severity,
                      std::string code,
                      std::string message) {
    result.issues.push_back({std::move(severity), std::move(code), std::move(message)});
}

inline bool valid_package_id(const std::string& id) {
    if (id.empty()) return false;
    for (char c : id) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '.' || c == '-' || c == '_' || c == ':'))
            return false;
    }
    return id.find("..") == std::string::npos;
}

inline bool valid_package_path_component(const std::string& id) {
    return valid_package_id(id)
        && id != "."
        && id != ".."
        && id.find(':') == std::string::npos
        && id.back() != '.';
}

inline bool valid_semverish(const std::string& version) {
    if (version.empty() || !std::isdigit(static_cast<unsigned char>(version.front())))
        return false;
    return std::all_of(version.begin(), version.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '+';
    });
}

inline fs::path resolve_manifest_path(const fs::path& input) {
    if (input.empty()) return {};
    std::error_code ec;
    if (fs::is_directory(input, ec)) return input / "pulp.package.json";
    return input;
}

inline fs::path manifest_root_for(const fs::path& manifest_path) {
    auto parent = manifest_path.parent_path();
    return parent.empty() ? fs::current_path() : parent;
}

inline bool path_value_exists(const fs::path& root, const std::string& rel) {
    if (rel.empty()) return false;
    fs::path p(rel);
    if (p.is_absolute()) return false;
    std::error_code ec;
    auto normalized = fs::weakly_canonical(root / p, ec);
    if (ec) normalized = (root / p).lexically_normal();
    auto root_norm = fs::weakly_canonical(root, ec);
    if (ec) root_norm = root.lexically_normal();
    if (!pulp::cli::fsutil::path_is_within(normalized, root_norm)) return false;
    return fs::exists(normalized);
}

inline std::string authoring_creator_type(const JsonValue& authoring) {
    if (auto created_by = string_field(authoring, "createdBy"); !created_by.empty())
        return created_by;
    if (auto* created_by = object_field(authoring, "createdBy")) {
        return string_field(*created_by, "type");
    }
    return {};
}

inline bool authoring_human_reviewed(const JsonValue& authoring) {
    if (has_field(authoring, "humanReviewed"))
        return bool_field(authoring, "humanReviewed", false);
    if (auto* human_review = object_field(authoring, "humanReview")) {
        return bool_field(*human_review, "reviewed", false);
    }
    return false;
}

}  // namespace pulp::cli::kit
