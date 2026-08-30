#include <pulp/authoring_capsule/manifest.hpp>

#include "canonical_json.hpp"

#include <pulp/runtime/crypto.hpp>

#include <choc/containers/choc_Value.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::authoring_capsule {
namespace {

namespace cv = choc::value;

// ── Enum vocabularies ───────────────────────────────────────────────────
//
// The JSON tokens are the wire contract and are frozen. They are held next
// to each other so a reader and a writer cannot drift: every token that
// parses is also a token that emits.

constexpr std::array<std::pair<std::string_view, Reproducibility>, 3> kReproducibilityTokens{{
    {"reproducible", Reproducibility::reproducible},
    {"best-effort", Reproducibility::best_effort},
    {"frozen-output-only", Reproducibility::frozen_output_only},
}};

constexpr std::array<std::pair<std::string_view, Canonicality>, 4> kCanonicalityTokens{{
    {"canonical-input", Canonicality::canonical_input},
    {"derived-output", Canonicality::derived_output},
    {"preview", Canonicality::preview},
    {"receipt", Canonicality::receipt},
}};

constexpr std::array<std::pair<std::string_view, SourceAvailability>, 4> kSourceAvailabilityTokens{{
    {"included", SourceAvailability::included},
    {"external", SourceAvailability::external},
    {"local-only", SourceAvailability::local_only},
    {"omitted", SourceAvailability::omitted},
}};

constexpr std::array<std::pair<std::string_view, Editability>, 2> kEditabilityTokens{{
    {"editable", Editability::editable},
    {"opaque", Editability::opaque},
}};

constexpr std::array<std::pair<std::string_view, Disclosure>, 5> kDisclosureTokens{{
    {"public", Disclosure::public_},
    {"recipient-scoped", Disclosure::recipient_scoped},
    {"private", Disclosure::private_},
    {"redacted", Disclosure::redacted},
    {"not_recorded", Disclosure::not_recorded},
}};

constexpr std::array<std::pair<std::string_view, Redistribution>, 3> kRedistributionTokens{{
    {"allowed", Redistribution::allowed},
    {"restricted", Redistribution::restricted},
    {"unknown", Redistribution::unknown},
}};

constexpr std::array<std::pair<std::string_view, RequiredFor>, 4> kRequiredForTokens{{
    {"play", RequiredFor::play},
    {"rebuild", RequiredFor::rebuild},
    {"remix", RequiredFor::remix},
    {"publish", RequiredFor::publish},
}};

template <typename Enum, std::size_t N>
std::optional<Enum> enum_from_token(const std::array<std::pair<std::string_view, Enum>, N>& table,
                                    std::string_view token) {
    for (const auto& entry : table)
        if (entry.first == token) return entry.second;
    return std::nullopt;
}

/// The tables are exhaustive over their enums, so a miss is a programming error
/// — a new enumerator added without its token. It still must not fall back to
/// `table.front()`: the first token is the permissive one for both
/// `Redistribution` ("allowed") and `SourceAvailability` ("included"), so that
/// fallback would silently upgrade an unmapped value into a grant nobody made.
/// An empty token is the honest answer, and it round-trips as a rejected enum
/// rather than as permission.
template <typename Enum, std::size_t N>
std::string_view enum_to_token(const std::array<std::pair<std::string_view, Enum>, N>& table,
                               Enum value) {
    for (const auto& entry : table)
        if (entry.second == value) return entry.first;
    return {};
}

// ── Top-level key vocabulary ────────────────────────────────────────────

/// Keys this reader understands and stores in `Manifest`. Anything else at
/// the top level is descriptive metadata a newer writer emitted, and is
/// preserved verbatim so a round-trip through this reader does not drop it.
constexpr std::array<std::string_view, 22> kKnownTopLevelKeys{
    "format",       "format_version", "profile",     "profile_version",
    "product",      "authoring_kind", "subtypes",    "topology",
    "required_capabilities",          "project_id",  "revision_id",
    "parent_revision",                "reproducibility",
    "compatibility",                  "files",       "dependencies",
    "title",        "created_at",     "exported_at", "provenance",
    "attestations", "distribution",
};

/// `completeness` is derived from the component rows and never authored, so a
/// declared value is consumed and discarded rather than round-tripped: keeping
/// it would let a stale claim outlive the rows it was computed from.
constexpr std::string_view kIgnoredTopLevelKey = "completeness";

bool is_known_top_level_key(std::string_view name) {
    if (name == kIgnoredTopLevelKey) return true;
    for (auto known : kKnownTopLevelKeys)
        if (known == name) return true;
    return false;
}

// ── Error construction ──────────────────────────────────────────────────

/// Every subject is a JSON pointer into the manifest, so a product can point
/// a person at the exact field that failed rather than at the whole document.
std::string ptr(std::string_view base, std::string_view key) {
    std::string out(base);
    out += '/';
    out += key;
    return out;
}

std::string ptr_index(std::string_view base, std::size_t index) {
    std::string out(base);
    out += '/';
    out += std::to_string(index);
    return out;
}

CapsuleError invalid(std::string subject) {
    CapsuleError error;
    error.status = CapsuleStatus::manifest_invalid;
    error.subject = std::move(subject);
    return error;
}

// ── Field readers ───────────────────────────────────────────────────────
//
// Each returns nullopt on success and leaves `out` untouched on failure, so
// the caller can chain them and surface the FIRST offending pointer.

std::optional<CapsuleError> read_string(const cv::ValueView& parent, std::string_view key,
                                        std::string_view base, bool required, bool allow_empty,
                                        std::string& out) {
    auto value = parent[key];
    if (value.isVoid()) {
        if (required) return invalid(ptr(base, key));
        return std::nullopt;
    }
    if (!value.isString()) return invalid(ptr(base, key));
    std::string text(value.getString());
    if (!allow_empty && text.empty()) return invalid(ptr(base, key));
    out = std::move(text);
    return std::nullopt;
}

std::optional<CapsuleError> read_u32(const cv::ValueView& parent, std::string_view key,
                                     std::string_view base, bool required, std::uint32_t& out) {
    auto value = parent[key];
    if (value.isVoid()) {
        if (required) return invalid(ptr(base, key));
        return std::nullopt;
    }
    if (!value.isInt()) return invalid(ptr(base, key));
    const auto n = value.get<std::int64_t>();
    if (n < 0 || n > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
        return invalid(ptr(base, key));
    out = static_cast<std::uint32_t>(n);
    return std::nullopt;
}

std::optional<CapsuleError> read_u64(const cv::ValueView& parent, std::string_view key,
                                     std::string_view base, bool required, std::uint64_t& out) {
    auto value = parent[key];
    if (value.isVoid()) {
        if (required) return invalid(ptr(base, key));
        return std::nullopt;
    }
    if (!value.isInt()) return invalid(ptr(base, key));
    const auto n = value.get<std::int64_t>();
    if (n < 0) return invalid(ptr(base, key));
    out = static_cast<std::uint64_t>(n);
    return std::nullopt;
}

std::optional<CapsuleError> read_bool(const cv::ValueView& parent, std::string_view key,
                                      std::string_view base, bool& out) {
    auto value = parent[key];
    if (value.isVoid()) return std::nullopt;
    if (!value.isBool()) return invalid(ptr(base, key));
    out = value.getBool();
    return std::nullopt;
}

std::optional<CapsuleError> read_string_array(const cv::ValueView& parent, std::string_view key,
                                              std::string_view base,
                                              std::vector<std::string>& out) {
    auto value = parent[key];
    if (value.isVoid()) return std::nullopt;
    if (!value.isArray()) return invalid(ptr(base, key));
    const std::string self = ptr(base, key);
    out.clear();
    out.reserve(value.size());
    for (std::uint32_t i = 0; i < value.size(); ++i) {
        auto element = value[i];
        if (!element.isString()) return invalid(ptr_index(self, i));
        out.emplace_back(element.getString());
    }
    return std::nullopt;
}

/// Read a profile-owned or descriptive subtree and store it as canonical JSON.
/// The layer does not interpret these; it only guarantees they survive a
/// round-trip in a form whose bytes are stable.
std::optional<CapsuleError> read_json_subtree(const cv::ValueView& parent, std::string_view key,
                                              std::string_view base, bool expect_array,
                                              std::string& out) {
    auto value = parent[key];
    if (value.isVoid()) return std::nullopt;
    const bool shape_ok = expect_array ? value.isArray() : value.isObject();
    if (!shape_ok) return invalid(ptr(base, key));
    auto canonical = detail::to_canonical_text(value, ptr(base, key));
    if (!canonical) return std::move(canonical).error();
    out = std::move(canonical).value();
    return std::nullopt;
}

template <typename Enum, std::size_t N>
std::optional<CapsuleError> read_enum(const cv::ValueView& parent, std::string_view key,
                                      std::string_view base,
                                      const std::array<std::pair<std::string_view, Enum>, N>& table,
                                      Enum& out) {
    auto value = parent[key];
    if (value.isVoid()) return std::nullopt;  // absent keeps the struct default
    if (!value.isString()) return invalid(ptr(base, key));
    auto parsed = enum_from_token(table, value.getString());
    if (!parsed) return invalid(ptr(base, key));
    out = *parsed;
    return std::nullopt;
}

/// Policy defaults are the closed ones: `unknown` redistribution and
/// `private` disclosure. An omitted policy therefore grants nothing.
std::optional<CapsuleError> read_policy(const cv::ValueView& row, std::string_view base,
                                        ComponentPolicy& out) {
    auto value = row["policy"];
    if (value.isVoid()) return std::nullopt;
    if (!value.isObject()) return invalid(ptr(base, "policy"));
    const std::string self = ptr(base, "policy");

    if (auto e = read_enum(value, "canonicality", self, kCanonicalityTokens, out.canonicality))
        return e;
    if (auto e = read_enum(value, "source_availability", self, kSourceAvailabilityTokens,
                           out.source_availability))
        return e;
    if (auto e = read_enum(value, "editability", self, kEditabilityTokens, out.editability))
        return e;
    if (auto e = read_enum(value, "disclosure", self, kDisclosureTokens, out.disclosure)) return e;
    if (auto e = read_enum(value, "redistribution", self, kRedistributionTokens, out.redistribution))
        return e;
    if (auto e = read_string(value, "license_expression", self, false, true, out.license_expression))
        return e;
    if (auto e = read_string(value, "license_notice_sha256", self, false, true,
                             out.license_notice_sha256))
        return e;
    if (auto e = read_string(value, "creator", self, false, true, out.creator)) return e;
    if (auto e = read_string(value, "source_uri", self, false, true, out.source_uri)) return e;
    if (auto e = read_bool(value, "attribution_required", self, out.attribution_required)) return e;

    auto required_for = value["required_for"];
    if (!required_for.isVoid()) {
        if (!required_for.isArray()) return invalid(ptr(self, "required_for"));
        const std::string list = ptr(self, "required_for");
        out.required_for.clear();
        out.required_for.reserve(required_for.size());
        for (std::uint32_t i = 0; i < required_for.size(); ++i) {
            auto element = required_for[i];
            if (!element.isString()) return invalid(ptr_index(list, i));
            auto parsed = enum_from_token(kRequiredForTokens, element.getString());
            if (!parsed) return invalid(ptr_index(list, i));
            out.required_for.push_back(*parsed);
        }
    }
    return std::nullopt;
}

std::optional<CapsuleError> read_files(const cv::ValueView& root, std::vector<FileEntry>& out) {
    auto value = root["files"];
    if (value.isVoid()) return std::nullopt;
    if (!value.isArray()) return invalid("/files");
    out.clear();
    out.reserve(value.size());
    for (std::uint32_t i = 0; i < value.size(); ++i) {
        auto row = value[i];
        const std::string self = ptr_index("/files", i);
        if (!row.isObject()) return invalid(self);

        FileEntry entry;
        if (auto e = read_string(row, "role", self, true, false, entry.role)) return e;
        if (auto e = read_string(row, "path", self, true, false, entry.path)) return e;
        if (auto e = read_string(row, "sha256", self, true, false, entry.sha256)) return e;
        if (auto e = read_u64(row, "bytes", self, true, entry.bytes)) return e;
        if (auto e = read_string(row, "media_type", self, true, false, entry.media_type)) return e;
        if (auto e = read_bool(row, "executable_data", self, entry.executable_data)) return e;
        if (auto e = read_policy(row, self, entry.policy)) return e;

        // A files[] row means the bytes are in this archive, so any other
        // availability is a contradiction. Admitting one would create a
        // component that can never resolve and can never be fetched, because
        // only a dependency row carries a provider. Anything not in the
        // archive belongs in dependencies[].
        if (entry.policy.source_availability != SourceAvailability::included) {
            CapsuleError error;
            error.status = CapsuleStatus::manifest_invalid;
            error.subject = self + "/policy/source_availability";
            error.required = "included";
            error.found = std::string(
                enum_to_token(kSourceAvailabilityTokens, entry.policy.source_availability));
            return error;
        }

        out.push_back(std::move(entry));
    }
    return std::nullopt;
}

/// The two admissible provider forms. An `https://` origin is authenticated in
/// transport and names somewhere a recipient can actually reach; a
/// `capsule-library:` locator names content a recipient may already hold,
/// without naming a machine at all. Everything else — `http://`, `file://`,
/// an absolute path, a bare hostname — is refused rather than carried, because
/// each either leaks the exporter's filesystem or invites an unauthenticated
/// fetch.
bool is_admissible_provider(std::string_view provider) {
    constexpr std::string_view kHttps = "https://";
    constexpr std::string_view kLibrary = "capsule-library:";
    const bool https = provider.size() > kHttps.size() && provider.substr(0, kHttps.size()) == kHttps;
    const bool library =
        provider.size() > kLibrary.size() && provider.substr(0, kLibrary.size()) == kLibrary;
    if (!https && !library) return false;
    // A control character in a URL is never meaningful and is how a log line or
    // a terminal gets forged, so it is refused here rather than sanitized later.
    return std::none_of(provider.begin(), provider.end(), [](unsigned char c) { return c < 0x20; });
}

std::optional<CapsuleError> read_dependencies(const cv::ValueView& root,
                                              std::vector<DependencyEntry>& out) {
    auto value = root["dependencies"];
    if (value.isVoid()) return std::nullopt;
    if (!value.isArray()) return invalid("/dependencies");
    out.clear();
    out.reserve(value.size());
    for (std::uint32_t i = 0; i < value.size(); ++i) {
        auto row = value[i];
        const std::string self = ptr_index("/dependencies", i);
        if (!row.isObject()) return invalid(self);

        DependencyEntry entry;
        if (auto e = read_string(row, "role", self, true, false, entry.role)) return e;
        if (auto e = read_string(row, "id", self, true, false, entry.id)) return e;
        if (auto e = read_string(row, "sha256", self, true, false, entry.sha256)) return e;
        if (auto e = read_u64(row, "bytes", self, true, entry.bytes)) return e;
        if (auto e = read_string(row, "media_type", self, true, false, entry.media_type)) return e;
        // An empty provider parses: a dependency may legitimately declare no
        // resolver, and it is then simply not resolvable. A provider that IS
        // declared must be one of the two admissible forms, because the string
        // is otherwise an unbounded channel — `file:///Users/someone/...` in a
        // shared capsule leaks the exporting machine, and `http://` invites a
        // retrieval nobody can authenticate. Which *host* is acceptable stays
        // a consumer policy; the shape is structural and belongs here.
        if (auto e = read_string(row, "provider", self, false, true, entry.provider)) return e;
        if (!entry.provider.empty() && !is_admissible_provider(entry.provider)) {
            CapsuleError error;
            error.status = CapsuleStatus::dependency_provider_denied;
            error.subject = ptr(self, "provider");
            error.required = "https:// or capsule-library:";
            error.found = entry.provider;
            return error;
        }
        if (auto e = read_bool(row, "required", self, entry.required)) return e;
        if (auto e = read_policy(row, self, entry.policy)) return e;
        out.push_back(std::move(entry));
    }
    return std::nullopt;
}

std::optional<CapsuleError> read_compatibility(const cv::ValueView& root, Compatibility& out) {
    auto value = root["compatibility"];
    if (value.isVoid()) return std::nullopt;
    if (!value.isObject()) return invalid("/compatibility");
    constexpr std::string_view self = "/compatibility";

    if (auto e = read_string(value, "min_product_version", self, false, true,
                             out.min_product_version))
        return e;
    if (auto e = read_string(value, "min_runtime_version", self, false, true,
                             out.min_runtime_version))
        return e;
    if (auto e = read_string(value, "schema_version", self, false, true, out.schema_version))
        return e;

    // Profile-owned floors ride along untouched under the same round-trip
    // guarantee the top level gives unknown descriptive keys.
    auto extra = cv::createObject("");
    for (std::uint32_t i = 0; i < value.size(); ++i) {
        auto member = value.getObjectMemberAt(i);
        const std::string_view name(member.name);
        if (name == "min_product_version" || name == "min_runtime_version" ||
            name == "schema_version")
            continue;
        extra.addMember(name, cv::Value(member.value));
    }
    auto canonical = detail::to_canonical_text(extra.getView(), self);
    if (!canonical) return std::move(canonical).error();
    out.extra_json = std::move(canonical).value();
    return std::nullopt;
}

// ── Emission ────────────────────────────────────────────────────────────

/// Re-hydrate a stored canonical-JSON subtree. A `Manifest` assembled by hand
/// can hold a blob this layer never parsed, so an unreadable one degrades to
/// its empty shape rather than making serialization fallible.
cv::Value subtree_value(const std::string& json, bool expect_array) {
    if (auto parsed = detail::parse_json(json)) {
        if (expect_array ? parsed->isArray() : parsed->isObject())
            return std::move(parsed).value();
    }
    return expect_array ? cv::createEmptyArray() : cv::createObject("");
}

cv::Value string_array_value(const std::vector<std::string>& values) {
    auto array = cv::createEmptyArray();
    for (const auto& value : values) array.addArrayElement(value);
    return array;
}

cv::Value policy_value(const ComponentPolicy& policy) {
    auto out = cv::createObject("");
    out.addMember("attribution_required", policy.attribution_required);
    out.addMember("canonicality", enum_to_token(kCanonicalityTokens, policy.canonicality));
    out.addMember("creator", policy.creator);
    out.addMember("disclosure", enum_to_token(kDisclosureTokens, policy.disclosure));
    out.addMember("editability", enum_to_token(kEditabilityTokens, policy.editability));
    out.addMember("license_expression", policy.license_expression);
    out.addMember("license_notice_sha256", policy.license_notice_sha256);
    out.addMember("redistribution", enum_to_token(kRedistributionTokens, policy.redistribution));

    auto required_for = cv::createEmptyArray();
    for (auto entry : policy.required_for)
        required_for.addArrayElement(enum_to_token(kRequiredForTokens, entry));
    out.addMember("required_for", std::move(required_for));

    out.addMember("source_availability",
                  enum_to_token(kSourceAvailabilityTokens, policy.source_availability));
    out.addMember("source_uri", policy.source_uri);
    return out;
}

cv::Value file_value(const FileEntry& file) {
    auto out = cv::createObject("");
    out.addMember("bytes", static_cast<std::int64_t>(file.bytes));
    // Covered by the identity: it is what tells a reader the payload contains
    // code, so a signed capsule must not be able to drop the flag and keep its
    // signature.
    out.addMember("executable_data", file.executable_data);
    out.addMember("media_type", file.media_type);
    out.addMember("path", file.path);
    out.addMember("policy", policy_value(file.policy));
    out.addMember("role", file.role);
    out.addMember("sha256", file.sha256);
    return out;
}

cv::Value dependency_value(const DependencyEntry& dependency) {
    auto out = cv::createObject("");
    out.addMember("bytes", static_cast<std::int64_t>(dependency.bytes));
    out.addMember("id", dependency.id);
    out.addMember("media_type", dependency.media_type);
    out.addMember("policy", policy_value(dependency.policy));
    out.addMember("provider", dependency.provider);
    out.addMember("required", dependency.required);
    out.addMember("role", dependency.role);
    out.addMember("sha256", dependency.sha256);
    return out;
}

/// Array order is the one thing key sorting cannot normalize, so `files` is
/// ordered by path in byte order before emission. Byte order is exactly what
/// `std::string` comparison gives (`char_traits<char>::compare` is memcmp).
std::vector<const FileEntry*> files_in_path_order(const std::vector<FileEntry>& files) {
    std::vector<const FileEntry*> ordered;
    ordered.reserve(files.size());
    for (const auto& file : files) ordered.push_back(&file);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const FileEntry* a, const FileEntry* b) { return a->path < b->path; });
    return ordered;
}

cv::Value files_value(const std::vector<FileEntry>& files) {
    auto array = cv::createEmptyArray();
    for (const auto* file : files_in_path_order(files))
        array.addArrayElement(file_value(*file));
    return array;
}

/// Sorted by `id`, for the same reason `files[]` is sorted by path: an array
/// whose order the exporter happened to produce would otherwise change the
/// identity of an unchanged project.
cv::Value dependencies_value(const std::vector<DependencyEntry>& dependencies) {
    std::vector<const DependencyEntry*> ordered;
    ordered.reserve(dependencies.size());
    for (const auto& dependency : dependencies) ordered.push_back(&dependency);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const DependencyEntry* a, const DependencyEntry* b) {
                         return a->id < b->id;
                     });

    auto array = cv::createEmptyArray();
    for (const auto* dependency : ordered) array.addArrayElement(dependency_value(*dependency));
    return array;
}

/// The three named floors are authoritative: a same-named key surviving in the
/// profile-owned extras is dropped rather than allowed to shadow the typed
/// field, so a floor can never be read two different ways.
cv::Value compatibility_value(const Compatibility& compatibility) {
    auto out = cv::createObject("");
    out.addMember("min_product_version", compatibility.min_product_version);
    out.addMember("min_runtime_version", compatibility.min_runtime_version);
    out.addMember("schema_version", compatibility.schema_version);

    auto extra = subtree_value(compatibility.extra_json, false);
    for (std::uint32_t i = 0; i < extra.size(); ++i) {
        auto member = extra.getObjectMemberAt(i);
        const std::string_view name(member.name);
        if (name == "min_product_version" || name == "min_runtime_version" ||
            name == "schema_version")
            continue;
        out.addMember(name, cv::Value(member.value));
    }
    return out;
}

/// Builds the manifest document. `include_identity_excluded` adds back exactly
/// the three fields the revision identity does not cover, so the document and
/// the digest payload come from one function and cannot drift apart.
///
/// Only three fields are excluded from the identity: `revision_id` (it is the
/// digest), `exported_at` (the one value that changes when nothing else did),
/// and `attestations` (signatures are computed over the digest). Everything
/// else is covered, `distribution` and each row's `executable_data` included —
/// those are what make a capsule Play-only and what tell the user a payload
/// contains code, so leaving them outside would let outer metadata be edited
/// on a validly signed capsule and still verify.
///
/// The determinism the format promises survives that breadth, because none of
/// the covered fields depends on the machine or the moment: they are authored
/// once and identical across two exports of the same project.
cv::Value build_document(const Manifest& manifest, bool include_identity_excluded) {
    auto out = cv::createObject("");
    out.addMember("authoring_kind", manifest.authoring_kind);
    out.addMember("compatibility", compatibility_value(manifest.compatibility));
    out.addMember("dependencies", dependencies_value(manifest.dependencies));
    out.addMember("files", files_value(manifest.files));
    out.addMember("format", manifest.format);
    out.addMember("format_version", static_cast<std::int64_t>(manifest.format_version));
    out.addMember("parent_revision", manifest.parent_revision);
    out.addMember("product", manifest.product);
    out.addMember("profile", manifest.profile);
    out.addMember("profile_version", static_cast<std::int64_t>(manifest.profile_version));
    out.addMember("project_id", manifest.project_id);
    out.addMember("reproducibility",
                  enum_to_token(kReproducibilityTokens, manifest.reproducibility));
    out.addMember("required_capabilities", string_array_value(manifest.required_capabilities));
    out.addMember("subtypes", string_array_value(manifest.subtypes));
    out.addMember("topology", subtree_value(manifest.topology_json, false));

    out.addMember("title", manifest.title);
    out.addMember("created_at", manifest.created_at);
    out.addMember("provenance", subtree_value(manifest.provenance_json, false));
    out.addMember("distribution", subtree_value(manifest.distribution_json, false));

    if (include_identity_excluded) {
        out.addMember("revision_id", manifest.revision_id);
        out.addMember("exported_at", manifest.exported_at);
        out.addMember("attestations", subtree_value(manifest.attestations_json, true));
    }

    // Unknown descriptive keys are re-emitted verbatim so a round-trip through
    // this reader is byte-identical, and they are covered by the identity for
    // the same reason every other descriptive field is: a key this build does
    // not understand is still content someone signed. A known key name is
    // skipped rather than re-added — the typed field is the authority, and
    // choc refuses a duplicate.
    auto unknown = subtree_value(manifest.unknown_optional_json, false);
    for (std::uint32_t i = 0; i < unknown.size(); ++i) {
        auto member = unknown.getObjectMemberAt(i);
        const std::string_view name(member.name);
        if (is_known_top_level_key(name)) continue;
        out.addMember(name, cv::Value(member.value));
    }
    return out;
}

}  // namespace

runtime::Result<std::string, CapsuleError> to_canonical_json(const Manifest& manifest) {
    // Only a `Manifest` assembled by hand reaches a failure here: a string this
    // layer never parsed that is not well-formed UTF-8, or a preserved subtree
    // nested past the canonical depth bound. It is reported rather than
    // swallowed so an exporter learns at export time, instead of handing the
    // user a file that fails admission somewhere else with no explanation.
    return detail::to_canonical_text(build_document(manifest,
                                                    /*include_identity_excluded=*/true)
                                         .getView());
}

runtime::Result<Manifest, CapsuleError> parse_manifest(std::string_view json) {
    using Result = runtime::Result<Manifest, CapsuleError>;

    try {
        auto root = detail::parse_json_object(json, "/");
        if (!root) return Result(runtime::Err(std::move(root).error()));

        const cv::ValueView& doc = root->getView();
        Manifest manifest;

        // Format identity first: a capsule from another format must be told
        // it is the wrong format, not that its fields are malformed.
        if (auto e = read_string(doc, "format", "", true, false, manifest.format))
            return Result(runtime::Err(*e));
        if (manifest.format != kFormatId) {
            CapsuleError error;
            error.status = CapsuleStatus::unsupported_format;
            error.subject = "/format";
            error.required = std::string(kFormatId);
            error.found = manifest.format;
            return Result(runtime::Err(std::move(error)));
        }

        if (auto e = read_u32(doc, "format_version", "", true, manifest.format_version))
            return Result(runtime::Err(*e));
        if (manifest.format_version == 0) return Result(runtime::Err(invalid("/format_version")));
        if (manifest.format_version > kFormatVersion) {
            CapsuleError error;
            error.status = CapsuleStatus::unsupported_format_version;
            error.subject = "/format_version";
            error.required = std::to_string(kFormatVersion);
            error.found = std::to_string(manifest.format_version);
            return Result(runtime::Err(std::move(error)));
        }

        // Routing. Whether a profile, product, or capability is *known* is the
        // profile registry's call; this function only proves the fields are
        // present and well-shaped.
        if (auto e = read_string(doc, "profile", "", true, false, manifest.profile))
            return Result(runtime::Err(*e));
        if (auto e = read_u32(doc, "profile_version", "", true, manifest.profile_version))
            return Result(runtime::Err(*e));
        if (manifest.profile_version == 0) return Result(runtime::Err(invalid("/profile_version")));
        if (auto e = read_string(doc, "product", "", true, false, manifest.product))
            return Result(runtime::Err(*e));
        if (auto e = read_string(doc, "authoring_kind", "", true, false, manifest.authoring_kind))
            return Result(runtime::Err(*e));
        if (auto e = read_string_array(doc, "subtypes", "", manifest.subtypes))
            return Result(runtime::Err(*e));
        if (auto e = read_json_subtree(doc, "topology", "", false, manifest.topology_json))
            return Result(runtime::Err(*e));
        if (auto e = read_string_array(doc, "required_capabilities", "",
                                       manifest.required_capabilities))
            return Result(runtime::Err(*e));

        // Identity.
        if (auto e = read_string(doc, "project_id", "", true, false, manifest.project_id))
            return Result(runtime::Err(*e));
        if (auto e = read_string(doc, "revision_id", "", false, true, manifest.revision_id))
            return Result(runtime::Err(*e));
        if (auto e = read_string(doc, "parent_revision", "", false, true,
                                 manifest.parent_revision))
            return Result(runtime::Err(*e));

        if (auto e = read_enum(doc, "reproducibility", "", kReproducibilityTokens,
                               manifest.reproducibility))
            return Result(runtime::Err(*e));
        if (auto e = read_compatibility(doc, manifest.compatibility))
            return Result(runtime::Err(*e));
        if (auto e = read_files(doc, manifest.files)) return Result(runtime::Err(*e));
        if (auto e = read_dependencies(doc, manifest.dependencies))
            return Result(runtime::Err(*e));

        // Descriptive.
        if (auto e = read_string(doc, "title", "", false, true, manifest.title))
            return Result(runtime::Err(*e));
        if (auto e = read_string(doc, "created_at", "", false, true, manifest.created_at))
            return Result(runtime::Err(*e));
        if (auto e = read_string(doc, "exported_at", "", false, true, manifest.exported_at))
            return Result(runtime::Err(*e));
        if (auto e = read_json_subtree(doc, "provenance", "", false, manifest.provenance_json))
            return Result(runtime::Err(*e));
        if (auto e = read_json_subtree(doc, "attestations", "", true, manifest.attestations_json))
            return Result(runtime::Err(*e));
        if (auto e = read_json_subtree(doc, "distribution", "", false, manifest.distribution_json))
            return Result(runtime::Err(*e));

        // Everything else at the top level is metadata a newer writer emitted.
        // It is captured in canonical form so a re-export reproduces it
        // byte-for-byte. An unknown *required* role, profile, or capability is
        // a different question and fails closed in the profile registry.
        auto unknown = cv::createObject("");
        for (std::uint32_t i = 0; i < doc.size(); ++i) {
            auto member = doc.getObjectMemberAt(i);
            if (is_known_top_level_key(std::string_view(member.name))) continue;
            unknown.addMember(member.name, cv::Value(member.value));
        }
        // The pointer root is empty because these members sit at the top level:
        // a rejected key then reports as `/key`, the pointer a person can look
        // up in the manifest, rather than `//key`.
        auto unknown_canonical = detail::to_canonical_text(unknown.getView());
        if (!unknown_canonical)
            return Result(runtime::Err(std::move(unknown_canonical).error()));
        manifest.unknown_optional_json = std::move(unknown_canonical).value();

        return Result(runtime::Ok(std::move(manifest)));
    } catch (const std::exception&) {
        // choc reports a type or structure violation by throwing. A capsule
        // that trips one is malformed, not merely unsupported.
        return Result(runtime::Err(invalid("/")));
    }
}

runtime::Result<std::string, CapsuleError> revision_digest(const Manifest& manifest) {
    using Result = runtime::Result<std::string, CapsuleError>;
    // A digest is an identity claim, so a manifest whose canonical form cannot
    // be produced gets no identity at all rather than a plausible constant one
    // that a lineage or attestation check would then compare against.
    auto canonical = detail::to_canonical_text(
        build_document(manifest, /*include_identity_excluded=*/false).getView());
    if (!canonical) return Result(runtime::Err(std::move(canonical).error()));
    return Result(runtime::Ok("sha256:" + runtime::sha256_hex(*canonical)));
}

}  // namespace pulp::authoring_capsule
