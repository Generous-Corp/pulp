// SPDX-License-Identifier: MIT
#include "kit_policy.hpp"

#include "kit_json_helpers.hpp"
#include "kit_manifest_validation.hpp"

#include <algorithm>
#include <cctype>
#include <pulp/runtime/crypto.hpp>

namespace pulp::cli::kit {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + c - 'a';
    if (c >= 'A' && c <= 'F')
        return 10 + c - 'A';
    return -1;
}

std::vector<std::uint8_t> hex_decode(std::string_view hex) {
    if (hex.size() % 2 != 0)
        return {};
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_value(hex[i]);
        const int lo = hex_value(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return {};
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

void add_issue(KitValidationResult& result, const std::string& code, const std::string& message) {
    result.issues.push_back({"error", code, message});
}

bool has_issue_code(const KitValidationResult& result, const std::string& code) {
    return std::any_of(result.issues.begin(), result.issues.end(),
                       [&](const KitIssue& issue) { return issue.code == code; });
}

std::string strings_json(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i)
            out += ",";
        out += json_string(values[i]);
    }
    return out + "]";
}

std::string signed_message(const KitSummary& summary, const std::string& canonical_sha256) {
    return "pulp-registry-manifest-v1\n" + summary.id + "\n" + summary.version + "\n" +
           canonical_sha256 + "\n";
}

} // namespace

void validate_publish_policy(KitValidationResult& result, const JsonValue& manifest) {
    if (auto* authoring = object_field(manifest, "authoring")) {
        if (authoring_creator_type(*authoring) == "agent" && !authoring_human_reviewed(*authoring))
            add_issue(result, "agent-review-required",
                      "agent-authored packages cannot publish without recorded human review");
    }
    if (auto* validation = object_field(manifest, "validation")) {
        if (field_array_empty(*validation, "profiles"))
            add_issue(result, "missing-validation-profile",
                      "`validation.profiles` must name at least one validation profile before "
                      "publishing");
    }
    auto* exports = object_field(manifest, "exports");
    auto* evidence = object_field(manifest, "evidence");
    if (!exports || field_array_empty(*exports, "licenses"))
        add_issue(result, "missing-notice-compatibility",
                  "`exports.licenses` must declare license/notice files before publishing");
    if (vector_contains(result.summary.kinds, "ui-kit")) {
        const bool has_screenshots = evidence && !field_array_empty(*evidence, "screenshots");
        const bool has_reports = evidence && !field_array_empty(*evidence, "reports");
        if (!has_screenshots && !has_reports)
            add_issue(result, "missing-ui-evidence",
                      "UI kits must declare screenshot/report evidence before publishing");
    }
    if (vector_contains(result.summary.kinds, "node-pack") &&
        (!exports || field_array_empty(*exports, "nodePackManifests")))
        add_issue(result, "missing-node-pack-manifest",
                  "node-pack kits must export at least one nodePackManifests entry");
    if (vector_contains(result.summary.kinds, "native-component")) {
        const bool has_headers = exports && !field_array_empty(*exports, "nativeComponentHeaders");
        const bool has_sources = exports && !field_array_empty(*exports, "nativeComponentSources");
        if (!has_headers && !has_sources)
            add_issue(result, "missing-native-component-files",
                      "native-component kits must export headers or sources before publishing");
    }
}

void validate_registry_manifest(KitValidationResult& result, const JsonValue& registry_manifest,
                                const std::string& expected_sha) {
    if (registry_manifest.type != JsonValue::Object) {
        add_issue(result, "invalid-registry-manifest", "registry manifest must be a JSON object");
        return;
    }
    if (string_field(registry_manifest, "schema") != "pulp-registry-manifest-v1")
        add_issue(result, "invalid-registry-manifest",
                  "registry manifest schema must be `pulp-registry-manifest-v1`");
    if (string_field(registry_manifest, "id") != result.summary.id)
        add_issue(result, "registry-manifest-id-mismatch",
                  "registry manifest id must match pulp.package.json");
    if (string_field(registry_manifest, "version") != result.summary.version)
        add_issue(result, "registry-manifest-version-mismatch",
                  "registry manifest version must match pulp.package.json");
    const auto canonical_sha = string_field(registry_manifest, "canonicalManifestSha256");
    if (canonical_sha != expected_sha)
        add_issue(result, "registry-manifest-digest-mismatch",
                  "registry manifest canonicalManifestSha256 must match pulp.package.json bytes");
    auto public_key = hex_decode(string_field(registry_manifest, "signerPublicKey"));
    auto signature = hex_decode(string_field(registry_manifest, "signature"));
    if (public_key.size() != pulp::runtime::ed25519_public_key_size)
        add_issue(result, "registry-manifest-public-key-invalid",
                  "registry manifest signerPublicKey must be a 32-byte Ed25519 key encoded as hex");
    if (signature.size() != pulp::runtime::ed25519_signature_size)
        add_issue(result, "registry-manifest-signature-invalid",
                  "registry manifest signature must be a 64-byte Ed25519 signature encoded as hex");
    if (public_key.size() == pulp::runtime::ed25519_public_key_size &&
        signature.size() == pulp::runtime::ed25519_signature_size && !canonical_sha.empty()) {
        const auto message = signed_message(result.summary, canonical_sha);
        if (!pulp::runtime::ed25519_verify(
                public_key.data(), public_key.size(), signature.data(), signature.size(),
                reinterpret_cast<const std::uint8_t*>(message.data()), message.size()))
            add_issue(result, "registry-manifest-signature-mismatch",
                      "registry manifest signature must verify the canonical package digest");
    }
}

std::string publish_badges_json(const KitValidationResult& result, bool registry_manifest_checked) {
    std::vector<std::string> badges;
    if (result.ok())
        badges.push_back("publish-ready");
    if (!has_issue_code(result, "missing-license-inventory"))
        badges.push_back("license-inventory");
    if (!has_issue_code(result, "missing-notice-compatibility"))
        badges.push_back("notice-compatibility");
    if (!has_issue_code(result, "agent-review-required"))
        badges.push_back("human-review");
    if (!has_issue_code(result, "missing-validation-profile"))
        badges.push_back("validation-profiles");
    if (!has_issue_code(result, "missing-ui-evidence") &&
        !has_issue_code(result, "missing-node-pack-manifest") &&
        !has_issue_code(result, "missing-native-component-files"))
        badges.push_back("kind-evidence");
    if (registry_manifest_checked && !has_issue_code(result, "missing-registry-manifest") &&
        !has_issue_code(result, "invalid-registry-manifest") &&
        !has_issue_code(result, "registry-manifest-id-mismatch") &&
        !has_issue_code(result, "registry-manifest-version-mismatch") &&
        !has_issue_code(result, "registry-manifest-digest-mismatch") &&
        !has_issue_code(result, "registry-manifest-public-key-invalid") &&
        !has_issue_code(result, "registry-manifest-signature-invalid") &&
        !has_issue_code(result, "registry-manifest-signature-mismatch"))
        badges.push_back("signed-canonical-manifest");
    return strings_json(badges);
}

std::string compatibility_json(const JsonValue& manifest) {
    const auto* requirements = object_field(manifest, "requires");
    const auto pulp_req = requirements ? string_field(*requirements, "pulp") : std::string{};
    const auto cpp = requirements && requirements->get("cpp") &&
                             requirements->get("cpp")->type == JsonValue::Number
                         ? requirements->get("cpp")->as_int()
                         : 0;
    const auto platforms =
        requirements ? string_array_field(*requirements, "platforms") : std::vector<std::string>{};
    int platform_score = static_cast<int>(platforms.size()) * 100 / 7;
    if (platform_score > 100)
        platform_score = 100;
    return "{\"requires_pulp\":" + json_string(pulp_req) +
           ",\"minimum_cpp\":" + std::to_string(cpp) + ",\"platforms\":" + strings_json(platforms) +
           ",\"platform_score\":" + std::to_string(platform_score) + "}";
}

} // namespace pulp::cli::kit
