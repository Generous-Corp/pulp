// GraphSerializer implementation: SignalGraph <-> .pulpgraph JSON.

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/control_index_nodes.hpp>
#include <pulp/host/processor_restore.hpp>
#include <pulp/host/sample_region_parameters.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/log.hpp>

#include "graph_load_refusal.hpp"
#include "graph_serializer_internal.hpp"
#include "graph_serializer_plugin_resolver.hpp"
#include "graph_serializer_regions.hpp"
#include "graph_serializer_test_access.hpp"
#include "spectral_graph_persistence.hpp"
#include "subgraph_voice_records.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <string_view>

namespace pulp::host {
namespace {

using detail::format_identity;
using detail::format_str;
using detail::identity_key;
using detail::kMaxSerializedConnections;
using detail::kMaxSerializedSampleRegionConnections;
using detail::kSavedIdentityV6;
using detail::kSavedIdentityV7;
using detail::kSavedIdentityV7FormatVersion;
using detail::structural_node_identities;
using detail::valid_authored_id;

std::function<void(const std::filesystem::path&)> g_after_first_read_hook;
std::atomic<bool> g_fail_next_authored_id_plan{false};
std::atomic<bool> g_fail_next_identity_record{false};
std::atomic<bool> g_fail_next_document_parse{false};
std::atomic<bool> g_fail_next_event_rebind{false};

// Bounds nesting depth and object member counts in one linear scan before the
// document reaches the parser. Each member of an object has exactly one ':'
// outside a string at that object's level.
GraphDocumentRefusal preflight_document_shape(std::string_view json) noexcept {
    bool in_string = false;
    bool escaped = false;
    std::size_t depth = 0;
    std::size_t total_members = 0;
    // Container kind and member count per depth; index 0 is the top level.
    bool is_object[GraphSerializer::max_document_depth + 1] = {};
    std::size_t members[GraphSerializer::max_document_depth + 1] = {};
    for (const auto character : json) {
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (character == '\\')
                escaped = true;
            else if (character == '"')
                in_string = false;
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{' || character == '[') {
            if (++depth > GraphSerializer::max_document_depth)
                return GraphDocumentRefusal::NestingLimitExceeded;
            is_object[depth] = character == '{';
            members[depth] = 0;
        } else if ((character == '}' || character == ']') && depth != 0) {
            --depth;
        } else if (character == ':' && depth != 0 && is_object[depth]) {
            if (++members[depth] > GraphSerializer::max_object_members ||
                ++total_members > GraphSerializer::max_document_members)
                return GraphDocumentRefusal::MemberLimitExceeded;
        }
    }
    return GraphDocumentRefusal::None;
}

template <typename V>
bool append_document_canonical(const V& value, std::string& output, GraphDocumentRefusal& refusal,
                               std::size_t depth) {
    if (depth > GraphSerializer::max_document_depth) {
        refusal = GraphDocumentRefusal::NestingLimitExceeded;
        return false;
    }
    if (value.isObject()) {
        std::vector<std::string> names;
        names.reserve(value.size());
        for (std::uint32_t i = 0; i < value.size(); ++i)
            names.emplace_back(value.getObjectMemberAt(i).name);
        std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) {
            return std::lexicographical_compare(
                a.begin(), a.end(), b.begin(), b.end(),
                [](unsigned char x, unsigned char y) { return x < y; });
        });
        if (std::adjacent_find(names.begin(), names.end()) != names.end()) {
            refusal = GraphDocumentRefusal::DuplicateJsonKey;
            return false;
        }
        output.push_back('{');
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i)
                output.push_back(',');
            output += choc::json::toString(choc::value::createString(names[i]), false);
            output.push_back(':');
            if (!append_document_canonical(value[names[i]], output, refusal, depth + 1))
                return false;
        }
        output.push_back('}');
        return true;
    }
    if (value.isArray()) {
        output.push_back('[');
        for (std::uint32_t i = 0; i < value.size(); ++i) {
            if (i)
                output.push_back(',');
            if (!append_document_canonical(value[i], output, refusal, depth + 1))
                return false;
        }
        output.push_back(']');
        return true;
    }
    if ((value.isFloat32() || value.isFloat64()) && !std::isfinite(value.getFloat64())) {
        refusal = GraphDocumentRefusal::NonFiniteNumber;
        return false;
    }
    output += choc::json::toString(value, false);
    return true;
}

// Tolerant numeric coercion — choc distinguishes int/float strictly and
// getFloat64() throws on integers, so wrap accesses to coerce the common
// "1.0 round-trips as int64" case. Templated so it accepts both Value
// and ValueView returned by operator[].
template <typename V>
inline double get_double(const V& v) {
    if (v.isFloat32() || v.isFloat64()) return v.getFloat64();
    if (v.isInt32() || v.isInt64()) return (double)v.getInt64();
    return 0.0;
}

// v4 adds the Processor persistence envelope.  The bounded serializer slice
// keeps the v2/v3 writers byte stable and teaches the reader the explicit
// v3 -> v4 migration; Processor emission is added by the follow-on runtime
// lane once the public registry contract is available.
// v5 is emitted only when a graph needs typed ports or Control edges. Older
// readers refuse the envelope before they could reinterpret those as Audio.
// v6 is emitted only for graphs carrying spectral nodes and embedded
// spectral_assets. A v5 reader would otherwise load those nodes as unresolved
// Custom nodes and drop the assets on re-save, so it must refuse the version.
// v7 is the saved-identity v7 document: the graph payload plus persisted
// authored node IDs. It is written only with the v7 envelope, so a reader that
// predates v7 refuses the document instead of loading it without its IDs.
constexpr int kFormatVersion = 7;
// The saved-identity envelope is format-independent and first shipped on v4,
// so inspection admits every format from v4 through the current writer. A v4
// envelope keeps its semantic digest when the writer advances to v5.
constexpr int kMinInspectedFormatVersion = 4;

struct GraphMigrationEntry {
    int from_version = 0;
    int to_version = 0;
    GraphSerializer::MigrationFn migration;
};

bool migrate_graph_v1_to_v2(const std::string& source_json,
                            std::string& migrated_json);
bool migrate_graph_v2_to_v3(const std::string& source_json, std::string& migrated_json);
bool migrate_graph_v3_to_v4(const std::string& source_json, std::string& migrated_json);
bool migrate_graph_v4_to_v5(const std::string& source_json, std::string& migrated_json);
bool migrate_graph_v5_to_v6(const std::string& source_json, std::string& migrated_json);
bool migrate_graph_v6_to_v7(const std::string& source_json, std::string& migrated_json);

std::vector<GraphMigrationEntry>& graph_migrations() {
    static std::vector<GraphMigrationEntry> migrations = {
        {1, 2, migrate_graph_v1_to_v2}, {2, 3, migrate_graph_v2_to_v3},
        {3, 4, migrate_graph_v3_to_v4}, {4, 5, migrate_graph_v4_to_v5},
        {5, 6, migrate_graph_v5_to_v6}, {6, 7, migrate_graph_v6_to_v7},
    };
    return migrations;
}

std::optional<int> graph_format_version(const choc::value::Value& root) {
    const auto& value = root["format_version"];
    if (value.isInt32() || value.isInt64()) {
        const auto raw = value.getInt64();
        if (raw < std::numeric_limits<int>::min()
            || raw > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
        return static_cast<int>(raw);
    }

    return std::nullopt;
}

bool replace_format_version(std::string& json,
                            int expected_version,
                            int replacement_version) {
    const auto key_pos = json.find("\"format_version\"");
    if (key_pos == std::string::npos) return false;
    const auto colon_pos = json.find(':', key_pos);
    if (colon_pos == std::string::npos) return false;

    auto value_begin = colon_pos + 1;
    while (value_begin < json.size()
           && (json[value_begin] == ' '
               || json[value_begin] == '\t'
               || json[value_begin] == '\r'
               || json[value_begin] == '\n')) {
        ++value_begin;
    }

    auto value_end = value_begin;
    if (value_end < json.size() && json[value_end] == '-') ++value_end;
    while (value_end < json.size()
           && json[value_end] >= '0'
           && json[value_end] <= '9') {
        ++value_end;
    }
    if (value_end == value_begin) return false;

    int parsed = 0;
    try {
        parsed = std::stoi(json.substr(value_begin, value_end - value_begin));
    } catch (...) {
        return false;
    }
    if (parsed != expected_version) return false;

    json.replace(value_begin, value_end - value_begin,
                 std::to_string(replacement_version));
    return true;
}

bool migrate_graph_v1_to_v2(const std::string& source_json,
                            std::string& migrated_json) {
    choc::value::Value root;
    try {
        root = choc::json::parse(source_json);
    } catch (...) {
        return false;
    }
    if (!root.isObject()) return false;
    auto version = graph_format_version(root);
    if (!version.has_value() || *version != 1) return false;

    migrated_json = source_json;
    return replace_format_version(migrated_json, 1, 2);
}

bool migrate_graph_v2_to_v3(const std::string& source_json, std::string& migrated_json) {
    migrated_json = source_json;
    try {
        const auto root = choc::json::parse(source_json);
        if (!root.isObject())
            return false;
        if (root.hasObjectMember("sample_regions")) {
            const auto& regions = root["sample_regions"];
            if (!regions.isArray() || regions.size() != 0)
                return false;
            return replace_format_version(migrated_json, 2, 3);
        }
    } catch (...) {
        return false;
    }
    const auto end = migrated_json.rfind('}');
    if (end == std::string::npos)
        return false;
    const auto body_end = migrated_json.find_last_not_of(" \t\r\n", end - 1);
    const bool comma = body_end != std::string::npos && migrated_json[body_end] != '{';
    migrated_json.insert(end, std::string(comma ? "," : "") + "\"sample_regions\":[]");
    return replace_format_version(migrated_json, 2, 3);
}

bool migrate_graph_v3_to_v4(const std::string& source_json, std::string& migrated_json) {
    // v4 is an additive envelope. A v3 graph without Processor records is
    // already structurally valid and must not be rewritten on disk by this
    // compatibility migration; only its schema version advances in memory.
    migrated_json = source_json;
    return replace_format_version(migrated_json, 3, 4);
}

bool migrate_graph_v4_to_v5(const std::string& source_json, std::string& migrated_json) {
    migrated_json = source_json;
    return replace_format_version(migrated_json, 4, 5);
}

bool migrate_graph_v5_to_v6(const std::string& source_json, std::string& migrated_json) {
    // v6 is additive: it only admits spectral nodes and spectral_assets, which
    // a v5 document cannot carry. Ordinary v5 files advance in memory only.
    migrated_json = source_json;
    return replace_format_version(migrated_json, 5, 6);
}

bool migrate_graph_v6_to_v7(const std::string& source_json, std::string& migrated_json) {
    // The loader reads a v7 payload exactly as v6; a v6 document advances in
    // memory only and gains no authored IDs from this step.
    migrated_json = source_json;
    return replace_format_version(migrated_json, 6, 7);
}

template <typename V> std::string_view identity_schema_of(const V& root) {
    if (!root.isObject() || !root.hasObjectMember("identity_schema") ||
        !root["identity_schema"].isString())
        return {};
    return root["identity_schema"].getString();
}

const GraphMigrationEntry* find_graph_migration(int from_version) {
    for (const auto& entry : graph_migrations()) {
        if (entry.from_version == from_version) {
            return &entry;
        }
    }
    return nullptr;
}

bool migrate_graph_json(std::string& json, std::string& error) {
    for (bool original = true;; original = false) {
        choc::value::Value root;
        try {
            root = choc::json::parse(json);
        } catch (const std::exception& e) {
            error = std::string("JSON parse failed: ") + e.what();
            return false;
        }

        if (!root.isObject()) {
            error = "root is not an object";
            return false;
        }
        if (!root.hasObjectMember("format_version")) {
            error = "missing graph format_version";
            return false;
        }

        auto version = graph_format_version(root);
        if (!version.has_value()) {
            error = "format_version is not an integer";
            return false;
        }

        if (original && *version == kSavedIdentityV7FormatVersion &&
            identity_schema_of(root) != kSavedIdentityV7) {
            error = "unsupported graph format_version 7 without the " +
                    std::string(kSavedIdentityV7) +
                    " envelope; format 7 is written only as a saved-identity v7 document";
            return false;
        }
        if (*version == kFormatVersion) {
            return true;
        }
        if (*version > kFormatVersion) {
            error = "unsupported graph format_version " + std::to_string(*version) +
                    "; upgrade Pulp, or re-export the graph at format " +
                    std::to_string(kFormatVersion) + " or lower";
            return false;
        }

        const auto* migration = find_graph_migration(*version);
        if (migration == nullptr) {
            error = "unsupported graph format_version "
                    + std::to_string(*version);
            return false;
        }

        if (migration->to_version <= *version
            || migration->to_version > kFormatVersion) {
            error = "graph migration does not move toward current format_version";
            return false;
        }

        std::string migrated;
        if (!migration->migration(json, migrated) || migrated.empty()) {
            error = "graph migration failed";
            return false;
        }

        choc::value::Value migrated_root;
        try {
            migrated_root = choc::json::parse(migrated);
        } catch (const std::exception& e) {
            error = std::string("graph migration produced invalid JSON: ") + e.what();
            return false;
        }

        if (!migrated_root.isObject()
            || !migrated_root.hasObjectMember("format_version")) {
            error = "graph migration did not produce expected format_version";
            return false;
        }

        auto migrated_version = graph_format_version(migrated_root);
        if (!migrated_version.has_value()
            || *migrated_version != migration->to_version) {
            error = "graph migration did not produce expected format_version";
            return false;
        }

        json = std::move(migrated);
    }
}

// The ID a migration assigns: a hash of the whole source document and the
// node's key in it (its v6 identity, or its persisted node id for a document
// without an envelope). The same document always migrates to the same IDs,
// and any difference between two documents changes every ID, so continuity
// is never guessed between independently edited copies.
std::string migrated_authored_id(std::string_view source_digest, std::string_view key) {
    std::string preimage = "pulp.graph.authored-id.migration.v1";
    preimage.push_back('\0');
    preimage += source_digest;
    preimage.push_back('\0');
    preimage += key;
    return "nid-" + runtime::sha256_hex(preimage).substr(0, 32);
}

// Digest of a source document for migrated_authored_id: its canonical form
// (sorted keys, so whitespace and member order do not matter), or its raw
// bytes when it cannot be canonicalized.
template <typename V> std::string migration_source_digest(const V& root, std::string_view raw) {
    std::string canonical;
    GraphDocumentRefusal refusal = GraphDocumentRefusal::None;
    if (!append_document_canonical(root, canonical, refusal, 0))
        canonical.assign(raw);
    return runtime::sha256_hex(canonical);
}

template <typename V>
bool append_semantic_canonical(const V& value, std::string& output, GraphDocumentRefusal& refusal,
                               std::size_t depth, bool in_node = false) {
    if (depth > GraphSerializer::max_document_depth) {
        refusal = GraphDocumentRefusal::NestingLimitExceeded;
        return false;
    }
    if (value.isObject()) {
        std::vector<std::string> names;
        for (std::uint32_t i = 0; i < value.size(); ++i) {
            const auto name = std::string(value.getObjectMemberAt(i).name);
            if (in_node && name == "layout")
                continue;
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        output.push_back('{');
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i)
                output.push_back(',');
            output += choc::json::toString(choc::value::createString(names[i]), false);
            output.push_back(':');
            if (!append_semantic_canonical(value[names[i]], output, refusal, depth + 1,
                                           in_node || names[i] == "nodes"))
                return false;
        }
        output.push_back('}');
        return true;
    }
    if (value.isArray()) {
        output.push_back('[');
        for (std::uint32_t i = 0; i < value.size(); ++i) {
            if (i)
                output.push_back(',');
            if (!append_semantic_canonical(value[i], output, refusal, depth + 1, in_node))
                return false;
        }
        output.push_back(']');
        return true;
    }
    output += choc::json::toString(value, false);
    return true;
}

template <typename V> bool is_json_integer(const V& value) {
    return value.isInt32() || value.isInt64();
}

template <typename V> bool is_finite_json_number(const V& value) {
    if (is_json_integer(value))
        return true;
    return (value.isFloat32() || value.isFloat64()) && std::isfinite(value.getFloat64());
}

template <typename V> bool is_instance_index(const V& value) {
    return is_json_integer(value) && value.getInt64() >= 0 &&
           value.getInt64() <= static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
}

template <typename V> bool is_nonempty_string(const V& value) {
    return value.isString() && !value.getString().empty();
}

// Authored subgraph instance records ride in the saved-identity envelope.
// Each record names logical node identities, never runtime NodeIds, and no
// node may belong to two instances. Values are data only here; template
// conformance is checked by the subgraph restore path.
template <typename V>
GraphDocumentRefusal validate_subgraph_records(const V& records,
                                               const std::unordered_set<std::string>& identities,
                                               std::string& detail) {
    if (!records.isArray()) {
        detail = "subgraphs must be an array of instance records";
        return GraphDocumentRefusal::SubgraphRecordInvalid;
    }
    std::unordered_set<std::string> instances;
    std::unordered_set<std::string> claimed;
    for (std::uint32_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        if (!record.isObject() || !record.hasObjectMember("template_identity") ||
            !is_nonempty_string(record["template_identity"]) ||
            !record.hasObjectMember("instance_identity") ||
            !is_nonempty_string(record["instance_identity"]) ||
            !record.hasObjectMember("instance_index") ||
            !is_instance_index(record["instance_index"]) || !record.hasObjectMember("members") ||
            !record["members"].isArray() || record["members"].size() == 0 ||
            !record.hasObjectMember("parameters") || !record["parameters"].isObject() ||
            !record.hasObjectMember("children") || !record["children"].isObject()) {
            detail = "subgraph record " + std::to_string(i) +
                     " requires template_identity, instance_identity, instance_index, members, "
                     "parameters, and children";
            return GraphDocumentRefusal::SubgraphRecordInvalid;
        }
        const auto instance = std::string(record["instance_identity"].getString());
        if (!instances.insert(instance).second) {
            detail = "duplicate subgraph instance identity: " + instance;
            return GraphDocumentRefusal::SubgraphBindingDuplicate;
        }
        const auto& members = record["members"];
        for (std::uint32_t m = 0; m < members.size(); ++m) {
            if (!is_nonempty_string(members[m])) {
                detail = "subgraph " + instance + " member must be a logical identity string";
                return GraphDocumentRefusal::SubgraphRecordInvalid;
            }
            const auto member = std::string(members[m].getString());
            if (!identities.contains(member)) {
                detail = "subgraph " + instance + " names unknown logical identity " + member;
                return GraphDocumentRefusal::SubgraphBindingDangling;
            }
            if (!claimed.insert(member).second) {
                detail = "logical identity " + member + " belongs to two subgraph instances";
                return GraphDocumentRefusal::SubgraphBindingDuplicate;
            }
        }
        const auto& parameters = record["parameters"];
        for (std::uint32_t p = 0; p < parameters.size(); ++p) {
            const auto member = parameters.getObjectMemberAt(p);
            if (std::string_view(member.name).empty() || !is_finite_json_number(member.value)) {
                detail = "subgraph " + instance + " parameter values must be finite numbers";
                return GraphDocumentRefusal::SubgraphRecordInvalid;
            }
        }
        const auto& children = record["children"];
        for (std::uint32_t c = 0; c < children.size(); ++c) {
            const auto child = children.getObjectMemberAt(c);
            if (std::string_view(child.name).empty() || !child.value.isObject() ||
                !child.value.hasObjectMember("template_identity") ||
                !is_nonempty_string(child.value["template_identity"]) ||
                !child.value.hasObjectMember("instance_index") ||
                !is_instance_index(child.value["instance_index"])) {
                detail = "subgraph " + instance +
                         " child records require template_identity and instance_index";
                return GraphDocumentRefusal::SubgraphRecordInvalid;
            }
        }
    }
    return GraphDocumentRefusal::None;
}

// The members of one event connection record. A record with any other
// member is refused, so a field this reader does not restore is never
// dropped silently.
constexpr std::string_view kEventConnectionMembers[] = {"producer", "consumer", "source_port",
                                                        "destination_port", "kind"};

// Event connection records name their producer and consumer by logical
// identity. Values are data only here; the binding rules (event-capable
// Custom endpoints, a feed-forward path, one connection per graph) are
// applied by SignalGraph::connect_graph_event when the document loads.
template <typename V>
GraphDocumentRefusal validate_event_connections(const V& records, bool saved_v7,
                                                const std::unordered_set<std::string>& identities,
                                                std::string& detail) {
    if (!saved_v7) {
        detail = "event_connections requires the saved-identity v7 envelope";
        return GraphDocumentRefusal::EventConnectionInvalid;
    }
    if (!records.isArray()) {
        detail = "event_connections must be an array of connection records";
        return GraphDocumentRefusal::EventConnectionInvalid;
    }
    std::set<std::tuple<std::string, std::string, std::int64_t, std::int64_t, std::int64_t>> seen;
    for (std::uint32_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        const auto where = "event connection " + std::to_string(i);
        if (!record.isObject() || record.size() != std::size(kEventConnectionMembers)) {
            detail = where + " requires exactly producer, consumer, source_port, "
                             "destination_port and kind";
            return GraphDocumentRefusal::EventConnectionInvalid;
        }
        for (const auto name : kEventConnectionMembers) {
            if (!record.hasObjectMember(name)) {
                detail = where + " is missing " + std::string(name);
                return GraphDocumentRefusal::EventConnectionInvalid;
            }
        }
        if (!is_nonempty_string(record["producer"]) || !is_nonempty_string(record["consumer"])) {
            detail = where + " producer and consumer must be logical identity strings";
            return GraphDocumentRefusal::EventConnectionInvalid;
        }
        for (const auto name : {"source_port", "destination_port", "kind"}) {
            if (!is_json_integer(record[name]) || record[name].getInt64() < 0 ||
                record[name].getInt64() > std::numeric_limits<std::uint8_t>::max()) {
                detail = where + " " + name + " must be an integer in 0..255";
                return GraphDocumentRefusal::EventConnectionInvalid;
            }
        }
        const auto producer = std::string(record["producer"].getString());
        const auto consumer = std::string(record["consumer"].getString());
        for (const auto* endpoint : {&producer, &consumer}) {
            if (!identities.contains(*endpoint)) {
                detail = where + " names unknown logical identity " + *endpoint;
                return GraphDocumentRefusal::EventConnectionDangling;
            }
        }
        if (!seen.emplace(producer, consumer, record["source_port"].getInt64(),
                          record["destination_port"].getInt64(), record["kind"].getInt64())
                 .second) {
            detail = where + " repeats the connection " + producer + " -> " + consumer;
            return GraphDocumentRefusal::EventConnectionDuplicate;
        }
    }
    return GraphDocumentRefusal::None;
}

const char* event_binding_name(GraphEventBindingResult result) noexcept {
    switch (result) {
    case GraphEventBindingResult::Accepted:
        return "accepted";
    case GraphEventBindingResult::Cardinality:
        return "cardinality";
    case GraphEventBindingResult::Binding:
        return "binding";
    case GraphEventBindingResult::Feedback:
        return "feedback";
    case GraphEventBindingResult::Mode:
        return "mode";
    }
    return "binding";
}

PluginFormat parse_format(std::string_view s) {
    if (s == "vst3") return PluginFormat::VST3;
    if (s == "au")   return PluginFormat::AudioUnit;
    if (s == "auv3") return PluginFormat::AudioUnitV3;
    if (s == "clap") return PluginFormat::CLAP;
    if (s == "builtin") return PluginFormat::BuiltIn;
    return PluginFormat::LV2;
}

bool parse_type(std::string_view s, NodeType& out) {
    if (s == "audio_in")  { out = NodeType::AudioInput; return true; }
    if (s == "audio_out") { out = NodeType::AudioOutput; return true; }
    if (s == "plugin")    { out = NodeType::Plugin; return true; }
    if (s == "gain")      { out = NodeType::Gain; return true; }
    if (s == "midi_in")   { out = NodeType::MidiInput; return true; }
    if (s == "midi_out")  { out = NodeType::MidiOutput; return true; }
    if (s == "custom")    { out = NodeType::Custom; return true; }
    return false;
}

// The authored identity key (without its ordinal) of one saved node record,
// computed from the record exactly as the loader rebuilds the node:
//   - a "processor" record restores a Plugin node with no unique id, and
//     must carry a name;
//   - a "plugin" record keeps its plugin.unique_id;
//   - a "custom" record uses custom.type_id and custom.version (1 when
//     absent), and an unknown type string restores as a custom node of that
//     type id at version 1, and must carry a name.
// Returns nullopt with `detail` set for a record the loader cannot rebuild
// as a node.
template <typename V>
std::optional<std::string> saved_record_identity_key(const V& node, std::string& detail) {
    const auto id = std::to_string(node["id"].getInt64());
    if (!node.hasObjectMember("type") || !node["type"].isString() ||
        !node.hasObjectMember("name") || !node["name"].isString()) {
        detail = "node record " + id + " requires a string type and name";
        return std::nullopt;
    }
    const std::string type(node["type"].getString());
    std::string name(node["name"].getString());
    if (type == "processor") {
        // The loader names an unnamed Processor node after its restored
        // descriptor, which inspection cannot know. The writer never records
        // an empty Processor name: a Processor node added without a name
        // takes its descriptor name, which descriptor validation requires.
        if (name.empty()) {
            detail = "processor node record " + id +
                     " requires a name; its restored identity cannot be determined";
            return std::nullopt;
        }
        return identity_key(NodeType::Plugin, name, {}, {}, 0);
    }
    NodeType parsed = NodeType::Custom;
    const bool known = parse_type(type, parsed);
    if (parsed == NodeType::Plugin) {
        if (!node.hasObjectMember("plugin") || !node["plugin"].isObject() ||
            !node["plugin"].hasObjectMember("unique_id") ||
            !node["plugin"]["unique_id"].isString()) {
            detail = "plugin node record " + id + " requires plugin.unique_id";
            return std::nullopt;
        }
        return identity_key(NodeType::Plugin, name, node["plugin"]["unique_id"].getString(), {}, 0);
    }
    if (parsed != NodeType::Custom)
        return identity_key(parsed, name, {}, {}, 0);
    std::string type_id = type;
    std::int64_t version = 1;
    if (known && node.hasObjectMember("custom")) {
        const auto& custom = node["custom"];
        if (!custom.isObject()) {
            detail = "custom node record " + id + " has a non-object custom member";
            return std::nullopt;
        }
        if (custom.hasObjectMember("type_id")) {
            if (!custom["type_id"].isString()) {
                detail = "custom node record " + id + " requires a string custom.type_id";
                return std::nullopt;
            }
            type_id = std::string(custom["type_id"].getString());
        }
        if (custom.hasObjectMember("version")) {
            if (!is_json_integer(custom["version"]) || custom["version"].getInt64() <= 0 ||
                custom["version"].getInt64() > std::numeric_limits<int>::max()) {
                detail = "custom node record " + id + " requires custom.version in 1..int max";
                return std::nullopt;
            }
            version = custom["version"].getInt64();
        }
    }
    // The loader names an unnamed custom node after its registered type's
    // default_name when one is set, and after its type id otherwise, and
    // inspection has no registry to tell the two apart. The writer never
    // records an empty custom name: a custom node added without a name takes
    // the default_name or type id at creation.
    if (name.empty()) {
        detail = "custom node record " + id +
                 " requires a name; its restored identity cannot be determined";
        return std::nullopt;
    }
    return identity_key(NodeType::Custom, name, {}, type_id, version);
}

std::string custom_type_label(std::string_view type_id, int version) {
    std::ostringstream oss;
    oss << type_id << "@" << version;
    return oss.str();
}

bool validate_generated_node_shape(NodeType type,
                                   std::string_view type_name,
                                   int inputs,
                                   int outputs,
                                   std::string& error) {
    if (inputs < 0 || outputs < 0) {
        error = "invalid generated node port count for "
              + std::string(type_name);
        return false;
    }

    auto fail_shape = [&] {
        error = "invalid generated node shape for " + std::string(type_name);
        return false;
    };

    switch (type) {
        case NodeType::AudioInput:
            return inputs == 0 && outputs > 0 ? true : fail_shape();
        case NodeType::AudioOutput:
            return inputs > 0 && outputs == 0 ? true : fail_shape();
        case NodeType::Gain:
            return inputs == 2 && outputs == 2 ? true : fail_shape();
        case NodeType::MidiInput:
            return inputs == 0 && outputs == 1 ? true : fail_shape();
        case NodeType::MidiOutput:
            return inputs == 1 && outputs == 0 ? true : fail_shape();
        case NodeType::Plugin:
        case NodeType::Custom:
            return true;
    }
    return fail_shape();
}

template <typename V> bool json_u64(const V& value, std::uint64_t max, std::uint64_t& out) {
    if (!(value.isInt32() || value.isInt64()))
        return false;
    const auto n = value.getInt64();
    if (n < 0 || static_cast<std::uint64_t>(n) > max)
        return false;
    out = static_cast<std::uint64_t>(n);
    return true;
}

template <typename V> bool json_float(const V& value, float& out) {
    if (!(value.isFloat32() || value.isFloat64() || value.isInt32() || value.isInt64()))
        return false;
    const auto d = get_double(value);
    if (!std::isfinite(d) || d < -std::numeric_limits<float>::max() ||
        d > std::numeric_limits<float>::max())
        return false;
    out = static_cast<float>(d);
    return true;
}

// Validate the wire form and calculate its exact decoded size before the
// decoder allocates either its cleaned input or output buffer.
std::optional<std::size_t> b64_decoded_size(std::string_view encoded) {
    std::size_t length = 0;
    std::size_t padding = 0;
    bool saw_padding = false;
    for (const char c : encoded) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        const bool alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (c == '=') {
            saw_padding = true;
            ++padding;
            if (padding > 2)
                return std::nullopt;
        } else if (!alphabet || saw_padding) {
            return std::nullopt;
        }
        ++length;
    }
    if (length % 4 != 0 || (padding != 0 && length < 4))
        return std::nullopt;
    return (length / 4) * 3 - padding;
}

std::optional<std::vector<uint8_t>> b64_decode(std::string_view s) {
    static int8_t kT[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) kT[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) kT[(unsigned char)a[i]] = (int8_t)i;
        init = true;
    }

    std::string clean;
    clean.reserve(s.size());
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        clean.push_back(c);
    }
    if (clean.empty()) return std::vector<uint8_t>{};
    if (clean.size() % 4 != 0) return std::nullopt;

    std::vector<uint8_t> out;
    out.reserve(clean.size() * 3 / 4);
    for (size_t i = 0; i < clean.size(); i += 4) {
        const bool final_quad = (i + 4 == clean.size());
        int vals[4] = {0, 0, 0, 0};
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = clean[i + static_cast<size_t>(j)];
            if (c == '=') {
                if (j < 2) return std::nullopt;
                ++padding;
                continue;
            }
            if (padding > 0) return std::nullopt;
            const int8_t d = kT[static_cast<unsigned char>(c)];
            if (d < 0) return std::nullopt;
            vals[j] = d;
        }
        if (padding > 2) return std::nullopt;
        if (padding > 0 && !final_quad) return std::nullopt;

        const uint32_t v = (static_cast<uint32_t>(vals[0]) << 18)
                         | (static_cast<uint32_t>(vals[1]) << 12)
                         | (static_cast<uint32_t>(vals[2]) << 6)
                         | static_cast<uint32_t>(vals[3]);
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
        if (padding < 2) out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        if (padding < 1) out.push_back(static_cast<uint8_t>(v & 0xff));
    }
    return out;
}

} // namespace

void GraphSerializerTestAccess::set_after_first_read(
    std::function<void(const std::filesystem::path&)> hook) {
    g_after_first_read_hook = std::move(hook);
}

void GraphSerializerTestAccess::fail_next_authored_id_plan() noexcept {
    g_fail_next_authored_id_plan.store(true, std::memory_order_release);
}

void GraphSerializerTestAccess::fail_next_identity_record() noexcept {
    g_fail_next_identity_record.store(true, std::memory_order_release);
}

void GraphSerializerTestAccess::fail_next_document_parse() noexcept {
    g_fail_next_document_parse.store(true, std::memory_order_release);
}

void GraphSerializerTestAccess::fail_next_spectral_candidate_build() noexcept {
    spectral_persistence::fail_next_candidate_build();
}

void GraphSerializerTestAccess::fail_next_event_rebind() noexcept {
    g_fail_next_event_rebind.store(true, std::memory_order_release);
}

const char* graph_document_refusal_name(GraphDocumentRefusal refusal) noexcept {
    switch (refusal) {
    case GraphDocumentRefusal::None:
        return "none";
    case GraphDocumentRefusal::EmptyDocument:
        return "empty_document";
    case GraphDocumentRefusal::DocumentTooLarge:
        return "document_too_large";
    case GraphDocumentRefusal::InvalidJson:
        return "invalid_json";
    case GraphDocumentRefusal::RootNotObject:
        return "root_not_object";
    case GraphDocumentRefusal::InvalidFormatVersion:
        return "invalid_format_version";
    case GraphDocumentRefusal::UnsupportedFutureVersion:
        return "unsupported_future_version";
    case GraphDocumentRefusal::LogicalIdentityMissing:
        return "logical_identity_missing";
    case GraphDocumentRefusal::UnsupportedDocumentProfile:
        return "unsupported_document_profile";
    case GraphDocumentRefusal::IdentityEnvelopeInvalid:
        return "identity_envelope_invalid";
    case GraphDocumentRefusal::IdentityBindingDangling:
        return "identity_binding_dangling";
    case GraphDocumentRefusal::IdentityBindingDuplicate:
        return "identity_binding_duplicate";
    case GraphDocumentRefusal::IdentityBindingUnknown:
        return "identity_binding_unknown";
    case GraphDocumentRefusal::DuplicateJsonKey:
        return "duplicate_json_key";
    case GraphDocumentRefusal::NestingLimitExceeded:
        return "nesting_limit_exceeded";
    case GraphDocumentRefusal::NonFiniteNumber:
        return "non_finite_number";
    case GraphDocumentRefusal::SubgraphRecordInvalid:
        return "subgraph_record_invalid";
    case GraphDocumentRefusal::SubgraphBindingDangling:
        return "subgraph_binding_dangling";
    case GraphDocumentRefusal::SubgraphBindingDuplicate:
        return "subgraph_binding_duplicate";
    case GraphDocumentRefusal::NodeIdInvalid:
        return "node_id_invalid";
    case GraphDocumentRefusal::MemberLimitExceeded:
        return "member_limit_exceeded";
    case GraphDocumentRefusal::EventConnectionInvalid:
        return "event_connection_invalid";
    case GraphDocumentRefusal::EventConnectionDangling:
        return "event_connection_dangling";
    case GraphDocumentRefusal::EventConnectionDuplicate:
        return "event_connection_duplicate";
    case GraphDocumentRefusal::AllocationFailed:
        return "allocation_failed";
    }
    return "invalid_document";
}

GraphDocumentInspection GraphSerializer::inspect_document(std::string_view json) {
    GraphDocumentInspection result;
    if (json.empty()) {
        result.refusal = GraphDocumentRefusal::EmptyDocument;
        result.detail = "document is empty";
        return result;
    }
    if (json.size() > max_file_bytes) {
        result.refusal = GraphDocumentRefusal::DocumentTooLarge;
        result.detail = "document exceeds the encoded size limit";
        return result;
    }
    if (const auto refusal = preflight_document_shape(json);
        refusal != GraphDocumentRefusal::None) {
        result.refusal = refusal;
        result.detail = graph_document_refusal_name(refusal);
        return result;
    }

    try {
        const auto root = choc::json::parse(json);
        if (!root.isObject()) {
            result.refusal = GraphDocumentRefusal::RootNotObject;
            result.detail = "saved document root is not an object";
            return result;
        }

        int format_version = 0;
        if (root.hasObjectMember("format_version")) {
            const auto version = graph_format_version(root);
            if (!version.has_value()) {
                result.refusal = GraphDocumentRefusal::InvalidFormatVersion;
                result.detail = "format_version must be a bounded integer";
                return result;
            }
            format_version = *version;
            const bool complete_saved_envelope = root.hasObjectMember("identity_schema") &&
                                                 root.hasObjectMember("logical_identities") &&
                                                 root.hasObjectMember("bindings");
            if (format_version < kMinInspectedFormatVersion && !complete_saved_envelope) {
                result.refusal = GraphDocumentRefusal::InvalidFormatVersion;
                result.detail = "saved document format is older than this inspector supports";
                return result;
            }
            if (format_version > kFormatVersion) {
                result.refusal = GraphDocumentRefusal::UnsupportedFutureVersion;
                result.detail = "saved document format is newer than this reader; upgrade Pulp, "
                                "or re-export at format " +
                                std::to_string(kFormatVersion) + " or lower";
                return result;
            }
            if (!root.hasObjectMember("identity_schema") &&
                !root.hasObjectMember("logical_identities")) {
                result.refusal = GraphDocumentRefusal::LogicalIdentityMissing;
                result.detail = "legacy graph has no explicit logical identity envelope";
                return result;
            }
        }

        std::string profile;
        if (root.hasObjectMember("document_schema")) {
            if (!root["document_schema"].isString()) {
                result.refusal = GraphDocumentRefusal::UnsupportedDocumentProfile;
                result.detail = "document_schema must be a string";
                return result;
            }
            profile = std::string(root["document_schema"].getString());
            if (profile != "pulp.graph.authored-io-gain.v1") {
                result.refusal = GraphDocumentRefusal::UnsupportedDocumentProfile;
                result.detail = "document_schema is not an admitted identity profile";
                return result;
            }
        }
        const auto admitted_envelope = [](std::string_view schema) {
            return schema == kSavedIdentityV6 || schema == kSavedIdentityV7;
        };
        if (root.hasObjectMember("identity_schema")) {
            if (!root["identity_schema"].isString() ||
                !admitted_envelope(root["identity_schema"].getString())) {
                result.refusal = GraphDocumentRefusal::UnsupportedDocumentProfile;
                result.detail = "identity_schema is not an admitted v6 or v7 envelope";
                return result;
            }
            profile = std::string(root["identity_schema"].getString());
        }
        if (root.hasObjectMember("logical_identities")) {
            const auto& identities = root["logical_identities"];
            if (!identities.isObject() || !identities.hasObjectMember("profile") ||
                !identities["profile"].isString() ||
                !admitted_envelope(identities["profile"].getString()) ||
                (!profile.empty() && identities["profile"].getString() != profile)) {
                result.refusal = GraphDocumentRefusal::UnsupportedDocumentProfile;
                result.detail = "logical_identities has no admitted profile matching "
                                "identity_schema";
                return result;
            }
            profile = std::string(identities["profile"].getString());
        }
        const bool saved_v7 = profile == kSavedIdentityV7;
        if ((root.hasObjectMember("identity_schema") ||
             root.hasObjectMember("logical_identities")) &&
            (format_version == kSavedIdentityV7FormatVersion) != saved_v7) {
            result.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
            result.detail = saved_v7 ? "a saved-identity v7 document requires format_version 7"
                                     : "format_version 7 requires the saved-identity v7 envelope";
            return result;
        }
        if (root.hasObjectMember("identity_schema") || root.hasObjectMember("logical_identities")) {
            if (!root.hasObjectMember("identity_schema") ||
                !root.hasObjectMember("logical_identities") ||
                !root["logical_identities"].isObject() ||
                !root["logical_identities"].hasObjectMember("identities") ||
                !root.hasObjectMember("bindings")) {
                result.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
                result.detail = "a saved-identity envelope requires identity_schema, "
                                "logical_identities and bindings";
                return result;
            }
            const auto& identities = root["logical_identities"]["identities"];
            const auto& bindings = root["bindings"];
            if (!identities.isArray() || !bindings.isObject()) {
                result.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
                result.detail =
                    "logical_identities.identities must be an array and bindings an object";
                return result;
            }
            std::unordered_set<std::string> known;
            std::unordered_map<std::string, NodeId> identity_node;
            std::unordered_set<NodeId> bound_nodes;
            std::unordered_set<NodeId> node_ids;
            std::unordered_set<std::string> authored_ids;
            std::unordered_map<NodeId, std::string> authored_of;
            // The loader refuses a node id outside 1..NodeId max and a
            // repeated one, so inspection refuses them too rather than
            // truncating or merging them.
            const auto refuse_node_id = [&](const std::string& where) {
                result.refusal = GraphDocumentRefusal::NodeIdInvalid;
                result.detail = where + " must be an integer node id in 1.." +
                                std::to_string(std::numeric_limits<NodeId>::max());
                return result;
            };
            const auto valid_node_id = [](const auto& value) {
                return is_json_integer(value) && value.getInt64() > 0 &&
                       static_cast<std::uint64_t>(value.getInt64()) <=
                           std::numeric_limits<NodeId>::max();
            };
            if (root.hasObjectMember("nodes") && root["nodes"].isArray()) {
                // The loader creates nodes in array order and numbers each
                // identity's ordinal in that order, while identities are
                // written in persisted id order. The writer lists node records
                // in ascending id order, so a record out of that order would
                // restore with a different ordinal than its identity names.
                std::int64_t previous_id = 0;
                for (std::uint32_t i = 0; i < root["nodes"].size(); ++i) {
                    const auto& node = root["nodes"][i];
                    if (!node.isObject() || !node.hasObjectMember("id") ||
                        !valid_node_id(node["id"]))
                        return refuse_node_id("nodes[" + std::to_string(i) + "].id");
                    if (!node_ids.insert(static_cast<NodeId>(node["id"].getInt64())).second) {
                        result.refusal = GraphDocumentRefusal::NodeIdInvalid;
                        result.detail =
                            "two node records share id " + std::to_string(node["id"].getInt64());
                        return result;
                    }
                    if (node["id"].getInt64() < previous_id) {
                        result.refusal = GraphDocumentRefusal::NodeIdInvalid;
                        result.detail = "nodes[" + std::to_string(i) + "].id " +
                                        std::to_string(node["id"].getInt64()) +
                                        " is out of order; node records must be listed in "
                                        "ascending id order";
                        return result;
                    }
                    previous_id = node["id"].getInt64();
                    // v7 binds identities to the authored_id each record
                    // carries. No v6 writer emitted one, so a v6 record that
                    // carries one is refused rather than guessed at.
                    if (!saved_v7) {
                        if (node.hasObjectMember("authored_id")) {
                            result.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
                            result.detail = "nodes[" + std::to_string(i) +
                                            "].authored_id requires the saved-identity v7 envelope";
                            return result;
                        }
                        continue;
                    }
                    if (!node.hasObjectMember("authored_id") || !node["authored_id"].isString() ||
                        !valid_authored_id(node["authored_id"].getString())) {
                        result.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
                        result.detail = "nodes[" + std::to_string(i) +
                                        "] requires an authored_id of 1..128 characters from "
                                        "[A-Za-z0-9._~-]";
                        return result;
                    }
                    const auto authored = std::string(node["authored_id"].getString());
                    if (!authored_ids.insert(authored).second) {
                        result.refusal = GraphDocumentRefusal::IdentityBindingDuplicate;
                        result.detail = "two node records carry authored_id " + authored;
                        return result;
                    }
                    authored_of.emplace(static_cast<NodeId>(node["id"].getInt64()), authored);
                }
            }
            for (std::uint32_t i = 0; i < identities.size(); ++i) {
                const auto& item = identities[i];
                if (!item.isObject() || !item.hasObjectMember("id") || !item["id"].isString() ||
                    !item.hasObjectMember("node_id")) {
                    result.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
                    result.detail = "identity entry requires string id and node_id";
                    return result;
                }
                const auto id = std::string(item["id"].getString());
                if (!valid_node_id(item["node_id"]))
                    return refuse_node_id("identity " + id + " node_id");
                if (!known.insert(id).second) {
                    result.refusal = GraphDocumentRefusal::IdentityBindingDuplicate;
                    result.detail = "duplicate logical identity: " + id;
                    return result;
                }
                const auto node_id = static_cast<NodeId>(item["node_id"].getInt64());
                if (!node_ids.contains(node_id)) {
                    result.refusal = GraphDocumentRefusal::IdentityBindingDangling;
                    result.detail = "identity binds to unknown node_id " + std::to_string(node_id) +
                                    "; re-export the graph with current node bindings";
                    return result;
                }
                if (!bound_nodes.insert(node_id).second) {
                    result.refusal = GraphDocumentRefusal::IdentityBindingDuplicate;
                    result.detail =
                        "two logical identities bind node_id " + std::to_string(node_id);
                    return result;
                }
                identity_node.emplace(id, node_id);
            }
            if (bound_nodes.size() != node_ids.size()) {
                result.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
                result.detail = "every serialized node requires a logical identity and binding";
                return result;
            }
            if (bindings.size() != identities.size()) {
                result.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
                result.detail = "bindings must name every logical identity exactly once";
                return result;
            }
            for (std::uint32_t i = 0; i < bindings.size(); ++i) {
                const auto name = std::string(bindings.getObjectMemberAt(i).name);
                if (!known.contains(name)) {
                    result.refusal = GraphDocumentRefusal::IdentityBindingUnknown;
                    result.detail = "binding references unknown logical identity: " + name;
                    return result;
                }
                if (!valid_node_id(bindings[name]))
                    return refuse_node_id("binding " + name);
                const auto node_id = static_cast<NodeId>(bindings[name].getInt64());
                if (!node_ids.contains(node_id)) {
                    result.refusal = GraphDocumentRefusal::IdentityBindingDangling;
                    result.detail = "binding references unknown node_id " +
                                    std::to_string(node_id) +
                                    "; re-export the graph with current node bindings";
                    return result;
                }
                if (node_id != identity_node.at(name)) {
                    result.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
                    result.detail = "binding for " + name + " names node_id " +
                                    std::to_string(node_id) +
                                    " but the identity entry binds a different node";
                    return result;
                }
            }
            // v7: every identity entry must spell the authored_id of the record
            // it binds. A document whose entries name other records (for
            // example two swapped IDs with consistent bindings) would otherwise
            // be diffed under false identities.
            if (saved_v7) {
                for (const auto& [name, node_id] : identity_node) {
                    const auto& authored = authored_of.at(node_id);
                    if (authored != name) {
                        result.refusal = GraphDocumentRefusal::IdentityBindingUnknown;
                        result.detail = "logical identity " + name + " binds node " +
                                        std::to_string(node_id) + ", whose authored_id is " +
                                        authored;
                        return result;
                    }
                }
            }
            // v6: rebuild every identity from the node records with the v6
            // writer's rule (ordinals assigned in persisted node id order) and
            // refuse a document whose identity strings name other nodes.
            // Without this, a document that swaps the identities of two nodes
            // of different types, with consistent bindings, would be diffed
            // under false identities while a load refuses it.
            if (!saved_v7 && root.hasObjectMember("nodes") && root["nodes"].isArray()) {
                std::vector<std::pair<NodeId, std::string>> keys;
                const auto& records = root["nodes"];
                keys.reserve(records.size());
                for (std::uint32_t i = 0; i < records.size(); ++i) {
                    std::string detail;
                    auto key = saved_record_identity_key(records[i], detail);
                    if (!key) {
                        result.refusal = GraphDocumentRefusal::IdentityBindingUnknown;
                        result.detail = std::move(detail);
                        return result;
                    }
                    keys.emplace_back(static_cast<NodeId>(records[i]["id"].getInt64()),
                                      std::move(*key));
                }
                std::sort(keys.begin(), keys.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
                std::unordered_map<std::string, std::size_t> ordinals;
                std::unordered_map<NodeId, std::string> expected;
                for (const auto& [node_id, key] : keys)
                    expected.emplace(node_id, format_identity(key, ordinals[key]++));
                for (const auto& [name, node_id] : identity_node) {
                    const auto& rebuilt = expected.at(node_id);
                    if (rebuilt != name) {
                        result.refusal = GraphDocumentRefusal::IdentityBindingUnknown;
                        result.detail = "logical identity " + name + " does not match node " +
                                        std::to_string(node_id) + ", whose identity is " + rebuilt;
                        return result;
                    }
                }
            }
            if (root.hasObjectMember("subgraphs")) {
                result.refusal = validate_subgraph_records(root["subgraphs"], known, result.detail);
                if (result.refusal != GraphDocumentRefusal::None)
                    return result;
            }
            if (root.hasObjectMember("event_connections")) {
                result.refusal = validate_event_connections(root["event_connections"], saved_v7,
                                                            known, result.detail);
                if (result.refusal != GraphDocumentRefusal::None)
                    return result;
            }
            if (root.hasObjectMember("voice_declarations")) {
                result.refusal =
                    detail::validate_voice_declarations(root.getView(), saved_v7, result.detail);
                if (result.refusal != GraphDocumentRefusal::None)
                    return result;
            }
        } else if (root.hasObjectMember("voice_declarations")) {
            result.refusal = GraphDocumentRefusal::SubgraphRecordInvalid;
            result.detail = "voice_declarations requires the saved-identity v7 envelope";
            return result;
        } else if (root.hasObjectMember("subgraphs")) {
            result.refusal = GraphDocumentRefusal::SubgraphRecordInvalid;
            result.detail = "subgraph records require a saved-identity envelope";
            return result;
        } else if (root.hasObjectMember("event_connections")) {
            result.refusal = GraphDocumentRefusal::EventConnectionInvalid;
            result.detail = "event_connections requires the saved-identity v7 envelope";
            return result;
        }
        if (format_version == 0 && profile.empty()) {
            result.refusal = GraphDocumentRefusal::LogicalIdentityMissing;
            result.detail = "document has no explicit logical identity profile";
            return result;
        }

        std::string canonical;
        GraphDocumentRefusal canonical_refusal = GraphDocumentRefusal::None;
        if (!append_document_canonical(root, canonical, canonical_refusal, 0)) {
            result.refusal = canonical_refusal;
            result.detail = graph_document_refusal_name(canonical_refusal);
            return result;
        }
        result.document.format_version_ = format_version;
        result.document.profile_ = std::move(profile);
        result.document.canonical_bytes_ = std::move(canonical);
        std::string semantic_canonical;
        if (!append_semantic_canonical(root, semantic_canonical, canonical_refusal, 0)) {
            result.refusal = canonical_refusal;
            result.detail = graph_document_refusal_name(canonical_refusal);
            return result;
        }
        std::string semantic_preimage = "pulp.graph.semantic.v6";
        semantic_preimage.push_back('\0');
        semantic_preimage += semantic_canonical;
        result.document.semantic_digest_ = "sha256:" + runtime::sha256_hex(semantic_preimage);
        return result;
    } catch (const std::bad_alloc&) {
        result.refusal = GraphDocumentRefusal::AllocationFailed;
        result.detail = "document inspection allocation failed";
    } catch (const std::exception& error) {
        const std::string message = error.what();
        result.refusal = (message.find("duplicate") != std::string::npos ||
                          message.find("Duplicate") != std::string::npos ||
                          message.find("already contains a member") != std::string::npos)
                             ? GraphDocumentRefusal::DuplicateJsonKey
                             : GraphDocumentRefusal::InvalidJson;
        result.detail = std::string("JSON parse failed: ") + message;
    }
    return result;
}

int GraphSerializer::current_format_version() {
    return kFormatVersion;
}

bool GraphSerializer::register_migration(int from_version,
                                         int to_version,
                                         MigrationFn migration) {
    if (from_version >= to_version
        || to_version > kFormatVersion
        || !migration) {
        return false;
    }

    if (find_graph_migration(from_version) != nullptr) {
        return false;
    }

    graph_migrations().push_back({from_version, to_version, std::move(migration)});
    return true;
}

GraphSerializer::LoadResult GraphSerializer::from_json(SignalGraph& graph, const std::string& json) {
    return from_json(graph, json, GraphLoadOptions{});
}

namespace {
// Lanes and the port fields the connect call records for that lane: MIDI
// edges ignore both ports and parameter edges ignore the destination port.
using ConnectionKey =
    std::tuple<NodeId, PortIndex, NodeId, PortIndex, bool, bool, bool, bool, bool, bool, bool>;

ConnectionKey connection_key(NodeId source, PortIndex source_port, NodeId dest, PortIndex dest_port,
                             bool feedback, bool midi, bool automation, bool audio_rate,
                             bool sidechain, bool control, bool spectral) {
    const bool parameter = automation || audio_rate;
    return {source,     midi ? PortIndex{0} : source_port,
            dest,       (midi || parameter) ? PortIndex{0} : dest_port,
            feedback,   midi,
            automation, audio_rate,
            sidechain,  control,
            spectral};
}

// Every connection a saved document records must exist in the restored graph.
// The tolerant loader skips a dangling or refused edge, so without this check a
// saved-identity load would report success with an edge missing. The one
// permitted omission is the loader's partial-load rule: a parameter edge from a
// valid source port into an unresolved plugin placeholder. A `spectral` flag is
// compared like the other lane flags, so a document that marks an edge spectral
// is refused when the edge was restored as plain audio. Returns a description of
// the first edge that is missing, or nullopt.
template <typename V>
std::optional<std::string>
find_unrestored_connection(const SignalGraph& graph, const V& root,
                           const std::unordered_map<NodeId, NodeId>& persisted_ids) {
    std::map<ConnectionKey, std::size_t> restored;
    for (const auto& c : graph.connections())
        ++restored[connection_key(c.source_node, c.source_port, c.dest_node, c.dest_port,
                                  c.feedback, c.midi, c.automation, c.audio_rate_modulation,
                                  c.sidechain, c.control_signal, c.spectral)];
    if (!root.hasObjectMember("connections"))
        return std::nullopt;
    const auto& connections = root["connections"];
    const auto flag = [](const auto& item, const char* name) {
        return item.hasObjectMember(name) && item[name].isBool() && item[name].getBool();
    };
    const auto port = [](const auto& item, const char* name) {
        return item.hasObjectMember(name) && item[name].isInt()
                   ? static_cast<PortIndex>(item[name].getInt64())
                   : PortIndex{0};
    };
    for (std::uint32_t i = 0; i < connections.size(); ++i) {
        const auto& item = connections[i];
        const auto describe = [&] { return "saved connection " + std::to_string(i); };
        if (!item.isObject() || !item.hasObjectMember("source_node") ||
            !item.hasObjectMember("dest_node") || !item["source_node"].isInt() ||
            !item["dest_node"].isInt())
            return describe() + " is malformed";
        const auto source = persisted_ids.find(static_cast<NodeId>(item["source_node"].getInt64()));
        const auto dest = persisted_ids.find(static_cast<NodeId>(item["dest_node"].getInt64()));
        if (source == persisted_ids.end() || dest == persisted_ids.end())
            return describe() + " names a node the document does not hold";
        const bool automation = flag(item, "automation");
        const bool audio_rate = flag(item, "audio_rate_modulation");
        const auto key = connection_key(
            source->second, port(item, "source_port"), dest->second, port(item, "dest_port"),
            flag(item, "feedback"), flag(item, "midi"), automation, audio_rate,
            flag(item, "sidechain"), flag(item, "control_signal"), flag(item, "spectral"));
        if (const auto it = restored.find(key); it != restored.end() && it->second != 0) {
            --it->second;
            continue;
        }
        const auto* target = graph.node(dest->second);
        const auto* source_node = graph.node(source->second);
        const bool valid_source =
            source_node != nullptr && source_node->num_output_ports > 0 &&
            port(item, "source_port") < static_cast<PortIndex>(source_node->num_output_ports);
        const bool placeholder = target != nullptr && target->type == NodeType::Plugin &&
                                 !target->plugin &&
                                 graph.processor_node_instance(target->id) == nullptr;
        if ((automation || audio_rate) && placeholder && valid_source)
            continue;
        return describe() + " was not restored (invalid port, cycle, or duplicate edge)";
    }
    return std::nullopt;
}
} // namespace

namespace {
// The authored ID each node record restores with, keyed by persisted id. A
// v7 record carries its own; any other document is migrated: the same rule
// as GraphSerializer::migrate_saved_document, keyed by the record's v6
// identity when the document has a v6 envelope, by its persisted id
// otherwise. Returns an error for a record whose ID is missing, malformed or
// repeated, or an authored_id outside a v7 document.
template <typename V>
std::optional<std::string> plan_authored_ids(const V& root, std::string_view raw,
                                             std::unordered_map<NodeId, std::string>& out) {
    if (!root.hasObjectMember("nodes") || !root["nodes"].isArray())
        return std::nullopt;
    const auto& records = root["nodes"];
    const bool v7 = graph_format_version(root).value_or(0) == kSavedIdentityV7FormatVersion;
    std::unordered_map<std::int64_t, std::string> v6_identity;
    std::string digest;
    if (!v7) {
        if (identity_schema_of(root) == kSavedIdentityV6 &&
            root.hasObjectMember("logical_identities") && root["logical_identities"].isObject() &&
            root["logical_identities"].hasObjectMember("identities") &&
            root["logical_identities"]["identities"].isArray()) {
            const auto& identities = root["logical_identities"]["identities"];
            for (std::uint32_t i = 0; i < identities.size(); ++i) {
                const auto& item = identities[i];
                if (item.isObject() && item.hasObjectMember("id") && item["id"].isString() &&
                    item.hasObjectMember("node_id") && is_json_integer(item["node_id"]))
                    v6_identity.emplace(item["node_id"].getInt64(),
                                        std::string(item["id"].getString()));
            }
        }
        digest = migration_source_digest(root, raw);
    }
    std::unordered_set<std::string> seen;
    for (std::uint32_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        if (!record.isObject() || !record.hasObjectMember("id") || !is_json_integer(record["id"]))
            continue; // the node loop reports the record
        const auto persisted = record["id"].getInt64();
        std::string id;
        if (v7) {
            if (!record.hasObjectMember("authored_id") || !record["authored_id"].isString() ||
                !valid_authored_id(record["authored_id"].getString()))
                return "node record " + std::to_string(persisted) +
                       " requires a valid authored_id in a format 7 document";
            id = std::string(record["authored_id"].getString());
        } else {
            if (record.hasObjectMember("authored_id"))
                return "node record " + std::to_string(persisted) +
                       " carries authored_id, which requires format_version 7 and the " +
                       std::string(kSavedIdentityV7) + " envelope";
            const auto found = v6_identity.find(persisted);
            id = migrated_authored_id(digest, found != v6_identity.end()
                                                  ? found->second
                                                  : "node_id:" + std::to_string(persisted));
        }
        if (!seen.insert(id).second)
            return "two node records restore with authored_id " + id;
        if (persisted > 0 &&
            static_cast<std::uint64_t>(persisted) <= std::numeric_limits<NodeId>::max())
            out.emplace(static_cast<NodeId>(persisted), std::move(id));
    }
    return std::nullopt;
}
} // namespace

std::optional<std::string>
GraphSerializer::unrestored_connection(const SignalGraph& graph, const std::string& json,
                                       const std::unordered_map<NodeId, NodeId>& persisted_ids) {
    try {
        const auto root = choc::json::parse(json);
        if (!root.isObject())
            return std::string("document is not a graph object");
        return find_unrestored_connection(graph, root, persisted_ids);
    } catch (const std::exception& error) {
        return std::string("document connections cannot be read: ") + error.what();
    }
}

GraphSerializer::LoadResult GraphSerializer::from_saved_document(SignalGraph& graph,
                                                                 const std::string& json,
                                                                 const GraphLoadOptions& options) {
    GraphSerializer::LoadResult result;
    const auto inspection = inspect_document(json);
    if (!inspection.ok()) {
        result.error =
            std::string(graph_document_refusal_name(inspection.refusal)) + ": " + inspection.detail;
        return result;
    }
    // A refusal after the load started leaves the graph and the result empty.
    const auto refuse = [&](std::string error) {
        detail::refuse_load(result, &graph, std::move(error));
        return result;
    };
    // Once from_json is called the caller's graph may already be replaced, so
    // any later failure, including an exception, goes through refuse().
    bool load_started = false;
    try {
        const auto root = choc::json::parse(json);
        if (!root.hasObjectMember("identity_schema")) {
            result.error = "identity_envelope_invalid: saved document has no identity envelope";
            return result;
        }
        const bool v7 = identity_schema_of(root) == kSavedIdentityV7;
        load_started = true;
        result = from_json(graph, json, options);
        if (!result.ok)
            return result;
        if (!result.unrestored_node_ids.empty())
            return refuse("node_not_restored: node record " +
                          std::to_string(result.unrestored_node_ids.front()) +
                          " could not be added to the graph");
        // Each identity must name the node its persisted id was restored as:
        // a v7 identity is that node's authored ID, a v6 identity its
        // structural identity (the v6 rule, unchanged). Checking only that
        // the identity exists somewhere would accept a document whose node
        // order swaps two same-key ordinals.
        const auto restored = v7 ? saved_node_identities(graph) : structural_node_identities(graph);
        const auto& identities = root["logical_identities"]["identities"];
        for (std::uint32_t i = 0; i < identities.size(); ++i) {
            const auto id = std::string(identities[i]["id"].getString());
            const auto persisted = static_cast<NodeId>(identities[i]["node_id"].getInt64());
            const auto live = result.persisted_node_ids.find(persisted);
            const auto live_identity = live == result.persisted_node_ids.end()
                                           ? restored.end()
                                           : restored.find(live->second);
            if (live_identity == restored.end() || live_identity->second != id)
                return refuse("identity_binding_unknown: logical identity " + id +
                              " does not name the node restored from node_id " +
                              std::to_string(persisted));
            if (g_fail_next_identity_record.exchange(false, std::memory_order_acq_rel))
                throw std::bad_alloc();
            result.identity_node_ids.emplace(id, live->second);
        }
        if (auto missing = find_unrestored_connection(graph, root, result.persisted_node_ids))
            return refuse("connection_not_restored: " + *missing);
        return result;
    } catch (const std::exception& error) {
        auto message = std::string("identity_envelope_invalid: ") + error.what();
        detail::refuse_load(result, load_started ? &graph : nullptr, std::move(message));
        return result;
    }
}

GraphSerializer::SavedDocumentMigration
GraphSerializer::migrate_saved_document(std::string_view json) {
    SavedDocumentMigration out;
    const auto inspection = inspect_document(json);
    if (!inspection.ok()) {
        out.refusal = inspection.refusal;
        out.detail = inspection.detail;
        return out;
    }
    if (inspection.document.profile() == kSavedIdentityV7) {
        out.document.assign(json);
        return out;
    }
    if (inspection.document.profile() != kSavedIdentityV6) {
        out.refusal = GraphDocumentRefusal::LogicalIdentityMissing;
        out.detail = "only a saved-identity v6 document migrates to v7";
        return out;
    }
    try {
        auto root = choc::json::parse(json);
        std::unordered_map<NodeId, std::string> authored;
        if (auto error = plan_authored_ids(root, json, authored)) {
            out.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
            out.detail = std::move(*error);
            return out;
        }
        std::map<std::string, std::string> renamed;
        const auto identities = root["logical_identities"]["identities"];
        auto rewritten = choc::value::createEmptyArray();
        auto bindings = choc::value::createObject("Bindings");
        for (std::uint32_t i = 0; i < identities.size(); ++i) {
            const auto old_id = std::string(identities[i]["id"].getString());
            const auto node_id = identities[i]["node_id"].getInt64();
            const auto& new_id = authored.at(static_cast<NodeId>(node_id));
            renamed.emplace(old_id, new_id);
            auto item = choc::value::createObject("Identity");
            item.addMember("id", new_id);
            item.addMember("node_id", node_id);
            rewritten.addArrayElement(item);
            bindings.addMember(new_id, node_id);
        }
        auto nodes = choc::value::createEmptyArray();
        for (std::uint32_t i = 0; i < root["nodes"].size(); ++i) {
            choc::value::Value record(root["nodes"][i]);
            record.addMember("authored_id",
                             authored.at(static_cast<NodeId>(record["id"].getInt64())));
            nodes.addArrayElement(record);
        }
        root.setMember("nodes", nodes);
        root.setMember("format_version", static_cast<int64_t>(kSavedIdentityV7FormatVersion));
        root.setMember("identity_schema", std::string(kSavedIdentityV7));
        auto logical = choc::value::createObject("LogicalIdentities");
        logical.addMember("profile", std::string(kSavedIdentityV7));
        logical.addMember("identities", rewritten);
        root.setMember("logical_identities", logical);
        root.setMember("bindings", bindings);
        if (root.hasObjectMember("subgraphs")) {
            auto records = choc::value::createEmptyArray();
            for (std::uint32_t i = 0; i < root["subgraphs"].size(); ++i) {
                choc::value::Value record(root["subgraphs"][i]);
                auto members = choc::value::createEmptyArray();
                for (std::uint32_t m = 0; m < record["members"].size(); ++m)
                    members.addArrayElement(
                        renamed.at(std::string(record["members"][m].getString())));
                record.setMember("members", members);
                records.addArrayElement(record);
            }
            root.setMember("subgraphs", records);
        }
        auto migrated = choc::json::toString(root, true);
        // The result must be a valid v7 document; anything else is a defect
        // in this function, refused rather than returned.
        const auto check = inspect_document(migrated);
        if (!check.ok() || check.document.profile() != kSavedIdentityV7) {
            out.refusal =
                check.ok() ? GraphDocumentRefusal::IdentityEnvelopeInvalid : check.refusal;
            out.detail = "migrated document failed v7 inspection: " + check.detail;
            return out;
        }
        out.document = std::move(migrated);
        out.identity_map.assign(renamed.begin(), renamed.end());
        out.migrated = true;
        return out;
    } catch (const std::bad_alloc&) {
        out.refusal = GraphDocumentRefusal::AllocationFailed;
        out.detail = "document migration allocation failed";
    } catch (const std::exception& error) {
        out.refusal = GraphDocumentRefusal::IdentityEnvelopeInvalid;
        out.detail = std::string("document migration failed: ") + error.what();
    }
    out.document.clear();
    return out;
}

bool GraphSerializer::read_file_bounded(const std::filesystem::path& path, std::string& json,
                                        std::string& error) {
    const auto fail = [&](std::string message) {
        error = std::move(message);
        json.clear();
        return false;
    };
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return fail("graph file cannot be opened: " + path.string());
    }
    if (size > max_file_bytes) {
        return fail("graph file exceeds " + std::to_string(max_file_bytes) + " byte limit");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return fail("graph file cannot be opened: " + path.string());
    }
    json.assign(static_cast<std::size_t>(size), '\0');
    if (!json.empty())
        input.read(json.data(), static_cast<std::streamsize>(json.size()));
    if (!input && !input.eof()) {
        return fail("graph file read failed: " + path.string());
    }
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count != json.size()) {
        return fail("graph file changed while reading: " + path.string());
    }
    if (g_after_first_read_hook) {
        auto hook = std::exchange(g_after_first_read_hook, {});
        hook(path);
    }
    ec.clear();
    const auto final_size = std::filesystem::file_size(path, ec);
    if (ec || final_size != size) {
        return fail("graph file changed while reading: " + path.string());
    }
    // A same-size replacement is invisible to the size checks above. Read the
    // current pathname again and compare its content before mutating the
    // caller's graph. This closes the replacement window for both truncating
    // writers and atomic same-size renames.
    std::ifstream verification(path, std::ios::binary);
    if (!verification) {
        return fail("graph file changed while reading: " + path.string());
    }
    std::string verified(static_cast<std::size_t>(size), '\0');
    if (!verified.empty())
        verification.read(verified.data(), static_cast<std::streamsize>(verified.size()));
    if (!verification && !verification.eof()) {
        return fail("graph file changed while reading: " + path.string());
    }
    if (static_cast<std::size_t>(verification.gcount()) != verified.size() || verified != json) {
        return fail("graph file changed while reading: " + path.string());
    }
    ec.clear();
    const auto verified_size = std::filesystem::file_size(path, ec);
    if (ec || verified_size != size) {
        return fail("graph file changed while reading: " + path.string());
    }
    return true;
}

GraphSerializer::LoadResult GraphSerializer::from_file(SignalGraph& graph,
                                                       const std::filesystem::path& path,
                                                       const GraphLoadOptions& options) {
    std::string json;
    LoadResult result;
    if (!read_file_bounded(path, json, result.error))
        return result;
    return from_json(graph, json, options);
}

GraphSerializer::LoadResult GraphSerializer::from_json(SignalGraph& graph, const std::string& json,
                                                       const GraphLoadOptions& options) {
    bool touched = false;
    LoadResult result;
    try {
        result = load_json_(graph, json, options, touched);
    } catch (const std::exception& error) {
        result.error = std::string("graph load failed: ") + error.what();
        result.ok = false;
    }
    if (!result.ok)
        detail::refuse_load(result, touched ? &graph : nullptr);
    return result;
}

GraphSerializer::LoadResult GraphSerializer::load_json_(SignalGraph& graph, const std::string& json,
                                                        const GraphLoadOptions& options,
                                                        bool& graph_touched) {
    LoadResult result;
    // Bound the document before any parse, so neither the spectral reader nor
    // the ordinary reader sees an oversized or quadratic-to-parse document.
    if (json.size() > max_file_bytes) {
        result.error =
            "graph document exceeds the " + std::to_string(max_file_bytes) + " byte limit";
        return result;
    }
    if (const auto refusal = preflight_document_shape(json);
        refusal != GraphDocumentRefusal::None) {
        result.error = std::string("graph document refused before parsing: ") +
                       graph_document_refusal_name(refusal);
        return result;
    }
    // A destination whose limits name a smaller block admits a spectral graph
    // at that block, the size it will be prepared for.
    auto spectral =
        spectral_persistence::read(json, std::clamp(graph.limits_.max_block_size, 1,
                                                    spectral_persistence::max_admission_block));
    if (spectral.handled) {
        if (!spectral.result.ok)
            return spectral.result;
        auto& prepared = *spectral.graph;
        // Each candidate node takes the authored ID its record restores with:
        // a v7 record's own, or the ID migrate_saved_document's rule derives
        // for a format 6 document. Planned and assigned on the isolated
        // candidate, so a failure here (including an allocation failure)
        // refuses before the caller's graph is touched.
        try {
            if (g_fail_next_authored_id_plan.exchange(false, std::memory_order_acq_rel))
                throw std::bad_alloc();
            const auto root = choc::json::parse(json);
            std::unordered_map<NodeId, std::string> authored;
            if (auto error = plan_authored_ids(root, json, authored)) {
                detail::refuse_load(spectral.result, nullptr, std::move(*error));
                return spectral.result;
            }
            std::unordered_map<NodeId, NodeId> persisted_of;
            for (const auto& [persisted, candidate] : spectral.result.persisted_node_ids)
                persisted_of.emplace(candidate, persisted);
            for (auto& n : prepared.nodes_) {
                const auto persisted = persisted_of.find(n.id);
                const auto id = persisted == persisted_of.end() ? authored.end()
                                                                : authored.find(persisted->second);
                if (id == authored.end()) {
                    detail::refuse_load(spectral.result, nullptr,
                                        "spectral node " + n.name + " has no planned authored_id");
                    return spectral.result;
                }
                n.authored_id = AuthoredNodeId(id->second);
            }
        } catch (const std::exception& error) {
            detail::refuse_load(spectral.result, nullptr,
                                std::string("authored_id planning failed: ") + error.what());
            return spectral.result;
        }
        SignalGraph::GraphMutationLock lock(graph);
        if (graph.refuse_spectral_mutation_()) {
            detail::refuse_load(spectral.result, nullptr,
                                "active spectral controller refuses graph loading; stop it first");
            return spectral.result;
        }
        prepared.set_limits(graph.limits_);
        if (!prepared.validate_generated_graph(std::min(4096, graph.limits_.max_block_size))
                 .accepted) {
            detail::refuse_load(spectral.result, nullptr,
                                "spectral graph exceeds destination graph limits");
            return spectral.result;
        }
        if (prepared.connections_.size() >
                std::numeric_limits<std::uint64_t>::max() - graph.next_connection_identity_ ||
            graph.authoring_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            detail::refuse_load(spectral.result, nullptr,
                                "spectral graph authoring identity exhausted");
            return spectral.result;
        }
        // No allocation or fallible construction remains after preflight.
        // Rebind handles so a candidate's ports cannot impersonate the target
        // graph, and old target handles remain stale across replacement.
        const auto incarnation = graph.authoring_generation_ + 1;
        for (auto& n : prepared.nodes_)
            for (auto* ports : {&n.input_ports, &n.output_ports})
                for (auto& p : *ports) {
                    p.owner = graph.diagnostic_graph_identity_;
                    p.incarnation = incarnation;
                }
        for (auto& id : prepared.connection_identities_)
            id = graph.next_connection_identity_++;
        graph.cancel_swap_edit_locked_();
        auto admitted_nodes = std::move(prepared.nodes_);
        prepared.nodes_.swap(graph.nodes_);
        graph.connections_.swap(prepared.connections_);
        graph.connection_identities_.swap(prepared.connection_identities_);
        graph.processor_nodes_.clear();
        graph.graph_event_edge_.reset();
        graph.sample_region_definitions_.clear();
        graph.sample_region_parameter_binding_ = nullptr;
        graph.prepared_sample_region_bank_.reset();
        graph.prepared_sample_regions_.clear();
        graph.next_id_ = prepared.next_id_;
        // Invalidate while authoring nodes are empty, exactly as clear() does,
        // so any previous typed-trigger attachment is retired too.
        graph.invalidate_live_locked_();
        graph.nodes_.swap(admitted_nodes);
        return spectral.result;
    }
    {
        SignalGraph::GraphMutationLock lock(graph);
        if (graph.refuse_spectral_mutation_()) {
            result.error = "active spectral controller refuses graph loading; stop it first";
            return result;
        }
    }
    detail::PluginNodeResolver plugin_resolver(options);

    int source_format_version = 0;
    std::unordered_map<NodeId, std::string> authored_ids;
    std::optional<std::string> authored_error;
    std::optional<choc::value::Value> original;
    // The original document decides the format version, the v7 envelope check
    // and the authored ID plan, so a failure to read it (including an
    // allocation failure) refuses the load before the graph is touched rather
    // than continuing as if the document had no version.
    try {
        if (g_fail_next_document_parse.exchange(false, std::memory_order_acq_rel))
            throw std::bad_alloc();
        original = choc::json::parse(json);
    } catch (const std::exception& e) {
        result.error = std::string("JSON parse failed: ") + e.what();
        return result;
    }
    if (original->isObject() && original->hasObjectMember("format_version"))
        source_format_version = graph_format_version(*original).value_or(0);
    std::string readable_json = json;
    if (!migrate_graph_json(readable_json, result.error)) {
        return result;
    }
    // Format 7 is written only as a complete saved-identity v7 document, and
    // the v7 envelope is written only at format 7. A document that declares
    // either (format_version 7, or the v7 identity_schema or profile at any
    // format_version) must pass the same format and envelope rules as
    // inspect_document before any of it loads; otherwise a v7 document
    // relabelled as an older format would load as legacy input and restore
    // under replacement identities.
    const auto declares_v7 = [](const choc::value::Value& root) {
        if (!root.isObject())
            return false;
        if (graph_format_version(root).value_or(0) == kSavedIdentityV7FormatVersion ||
            identity_schema_of(root) == kSavedIdentityV7)
            return true;
        if (!root.hasObjectMember("logical_identities"))
            return false;
        const auto& identities = root["logical_identities"];
        return identities.isObject() && identities.hasObjectMember("profile") &&
               identities["profile"].isString() &&
               identities["profile"].getString() == kSavedIdentityV7;
    };
    if (declares_v7(*original)) {
        const auto inspection = inspect_document(json);
        if (!inspection.ok()) {
            result.error = std::string(graph_document_refusal_name(inspection.refusal)) + ": " +
                           inspection.detail;
            return result;
        }
        if (inspection.document.profile() != kSavedIdentityV7) {
            result.error = "unsupported graph format_version 7 without the " +
                           std::string(kSavedIdentityV7) + " envelope";
            return result;
        }
    }
    if (original->isObject() && original->hasObjectMember("event_connections") &&
        !declares_v7(*original)) {
        result.error = "event_connection_invalid: event_connections requires the "
                       "saved-identity v7 envelope";
        return result;
    }
    if (original->isObject()) {
        // A planning failure (for example an allocation failure) must not
        // look like success with freshly minted IDs; refuse before the graph
        // is touched.
        try {
            if (g_fail_next_authored_id_plan.exchange(false, std::memory_order_acq_rel))
                throw std::bad_alloc();
            authored_error = plan_authored_ids(*original, json, authored_ids);
        } catch (const std::exception& error) {
            result.error = std::string("authored_id planning failed: ") + error.what();
            return result;
        }
    }
    if (authored_error) {
        result.error = std::move(*authored_error);
        return result;
    }

    choc::value::Value root;
    try {
        root = choc::json::parse(readable_json);
    } catch (const std::exception& e) {
        result.error = std::string("JSON parse failed: ") + e.what();
        return result;
    }
    if (!root.isObject()) {
        result.error = "root is not an object";
        return result;
    }
    // Clear only once the document parses, so malformed input leaves the
    // caller's graph as it was, as a refused spectral document does. From
    // here on from_json clears the graph again on any refusal.
    graph_touched = true;
    graph.clear();

    struct ParsedRegion {
        SampleRegionDefinition definition;
    };
    std::vector<ParsedRegion> parsed_regions;
    std::vector<Connection> parsed_connections;
    bool has_typed_custom_port_payload = false;
    const bool has_region_records = root.hasObjectMember("sample_regions") &&
                                    root["sample_regions"].isArray() &&
                                    root["sample_regions"].size() != 0;
    try {
        if (root.hasObjectMember("connections")) {
            const auto& conns = root["connections"];
            if (has_region_records && conns.size() > kMaxSerializedSampleRegionConnections) {
                result.error = "too many connections";
                return result;
            }
            parsed_connections.reserve(conns.size());
            for (uint32_t i = 0; i < conns.size(); ++i) {
                const auto& cv = conns[i];
                std::uint64_t n = 0;
                Connection c{};
                if (!json_u64(cv["source_node"], std::numeric_limits<NodeId>::max(), n) ||
                    (has_region_records && n == 0)) {
                    result.error = "invalid source_node";
                    return result;
                }
                c.source_node = (NodeId)n;
                if (!json_u64(cv["source_port"], std::numeric_limits<PortIndex>::max(), n)) {
                    result.error = "invalid source_port";
                    return result;
                }
                c.source_port = (PortIndex)n;
                if (!json_u64(cv["dest_node"], std::numeric_limits<NodeId>::max(), n) ||
                    (has_region_records && n == 0)) {
                    result.error = "invalid dest_node";
                    return result;
                }
                c.dest_node = (NodeId)n;
                if (!json_u64(cv["dest_port"], std::numeric_limits<PortIndex>::max(), n)) {
                    result.error = "invalid dest_port";
                    return result;
                }
                c.dest_port = (PortIndex)n;
                if (!cv["feedback"].isBool() || !cv["midi"].isBool() ||
                    !cv["automation"].isBool()) {
                    result.error = "invalid connection flags";
                    return result;
                }
                c.feedback = cv["feedback"].getBool();
                c.midi = cv["midi"].getBool();
                c.automation = cv["automation"].getBool();
                if ((cv.hasObjectMember("audio_rate_modulation") &&
                     !cv["audio_rate_modulation"].isBool()) ||
                    (cv.hasObjectMember("sidechain") && !cv["sidechain"].isBool()) ||
                    (cv.hasObjectMember("control_signal") && !cv["control_signal"].isBool())) {
                    result.error = "invalid connection flags";
                    return result;
                }
                c.audio_rate_modulation = cv.hasObjectMember("audio_rate_modulation")
                                              ? cv["audio_rate_modulation"].getBool()
                                              : false;
                c.sidechain = cv.hasObjectMember("sidechain") ? cv["sidechain"].getBool() : false;
                c.control_signal =
                    cv.hasObjectMember("control_signal") ? cv["control_signal"].getBool() : false;
                if (c.control_signal && source_format_version < 5) {
                    result.error = "Control connection requires graph format v5";
                    return result;
                }
                if (c.control_signal && (c.feedback || c.midi || c.automation || c.sidechain)) {
                    result.error = "Control connection has conflicting lane flags";
                    return result;
                }
                if (c.automation || c.audio_rate_modulation) {
                    if (!json_u64(cv["auto_param_id"], std::numeric_limits<std::uint32_t>::max(),
                                  n)) {
                        result.error = "invalid automation parameter";
                        return result;
                    }
                    c.automation_param_id = (std::uint32_t)n;
                    if (!json_float(cv["auto_range_lo"], c.automation_range_lo) ||
                        !json_float(cv["auto_range_hi"], c.automation_range_hi) ||
                        !json_float(cv["auto_smoothing"], c.automation_smoothing_ms) ||
                        !json_u64(cv["auto_mix"], 255, n)) {
                        result.error = "invalid automation values";
                        return result;
                    }
                    c.automation_mix = (AutomationMix)(std::uint8_t)n;
                }
                parsed_connections.push_back(c);
            }
        }
        if (root.hasObjectMember("sample_regions")) {
            const auto& regions = root["sample_regions"];
            if (!regions.isArray() || regions.size() > 16) {
                result.error = "invalid sample_regions";
                return result;
            }
            SampleRegionParserShape shape{};
            shape.regions = regions.size();
            std::unordered_set<SampleRegionId> region_ids;
            for (uint32_t i = 0; i < regions.size(); ++i) {
                const auto& rv = regions[i];
                if (!rv.isObject()) {
                    result.error = "invalid sample region";
                    return result;
                }
                ParsedRegion parsed;
                auto& d = parsed.definition;
                std::uint64_t n = 0;
                if (!json_u64(rv["id"], std::numeric_limits<SampleRegionId>::max(), n) || n == 0 ||
                    !region_ids.insert((SampleRegionId)n).second) {
                    result.error = "invalid or duplicate sample region id";
                    return result;
                }
                d.region_id = (SampleRegionId)n;
                if (!rv["members"].isArray() || rv["members"].size() > 64) {
                    result.error = "invalid sample region members";
                    return result;
                }
                for (uint32_t j = 0; j < rv["members"].size(); ++j) {
                    const auto& mv = rv["members"][j];
                    SampleRegionKernelNode m;
                    if (!json_u64(mv["node"], std::numeric_limits<NodeId>::max(), n) || n == 0) {
                        result.error = "invalid sample region member node";
                        return result;
                    }
                    m.node = (NodeId)n;
                    m.type_id = mv["type_id"].getString();
                    if (!json_u64(mv["version"], std::numeric_limits<int>::max(), n) || n == 0) {
                        result.error = "invalid sample region member version";
                        return result;
                    }
                    m.version = (int)n;
                    if (!json_u64(mv["config_kind"], 255, n) || n == 0 ||
                        (n > (std::uint64_t)SampleKernelConfigKind::PromotedParameterId)) {
                        result.error = "invalid sample kernel config kind";
                        return result;
                    }
                    m.config.kind = (SampleKernelConfigKind)(std::uint8_t)n;
                    if (!json_u64(mv["config_value"], std::numeric_limits<std::uint32_t>::max(),
                                  n)) {
                        result.error = "invalid sample kernel config value";
                        return result;
                    }
                    m.config.boundary_index_or_parameter_id = (std::uint32_t)n;
                    if (!json_float(mv["constant"], m.config.constant)) {
                        result.error = "invalid sample kernel constant";
                        return result;
                    }
                    d.members.push_back(std::move(m));
                }
                auto parse_ids = [&](const char* key, std::vector<NodeId>& out) -> bool {
                    if (!rv[key].isArray() || rv[key].size() > 8)
                        return false;
                    for (uint32_t j = 0; j < rv[key].size(); ++j) {
                        if (!json_u64(rv[key][j], std::numeric_limits<NodeId>::max(), n) || n == 0)
                            return false;
                        out.push_back((NodeId)n);
                    }
                    return true;
                };
                if (!parse_ids("input_boundaries", d.input_boundaries) ||
                    !parse_ids("output_boundaries", d.output_boundaries)) {
                    result.error = "invalid sample region boundaries";
                    return result;
                }
                const auto& ps = rv["promoted_parameters"];
                if (!ps.isArray() || ps.size() > 16) {
                    result.error = "invalid promoted parameters";
                    return result;
                }
                for (uint32_t j = 0; j < ps.size(); ++j) {
                    const auto& pv = ps[j];
                    SampleRegionPromotedParameter p;
                    if (!json_u64(pv["param_id"], std::numeric_limits<state::ParamID>::max(), n) ||
                        n == 0) {
                        result.error = "invalid promoted parameter id";
                        return result;
                    }
                    p.param_id = (state::ParamID)n;
                    p.key = pv["key"].getString();
                    p.name = pv["name"].getString();
                    p.unit = pv["unit"].getString();
                    if (!json_float(pv["min"], p.range.min) ||
                        !json_float(pv["max"], p.range.max) ||
                        !json_float(pv["default"], p.range.default_value) ||
                        !json_float(pv["step"], p.range.step) ||
                        !json_float(pv["skew"], p.range.skew) || !pv["symmetric_skew"].isBool() ||
                        !json_u64(pv["rate"], 255, n) ||
                        n != (std::uint64_t)state::ParamRate::ControlRate ||
                        !json_float(pv["smoothing"], p.smoothing_ramp_seconds) ||
                        !json_u64(pv["bound_node"], std::numeric_limits<NodeId>::max(), n) ||
                        n == 0) {
                        result.error = "invalid promoted parameter metadata";
                        return result;
                    }
                    p.range.symmetric_skew = pv["symmetric_skew"].getBool();
                    p.rate = state::ParamRate::ControlRate;
                    p.bound_node_id = (NodeId)n;
                    if (!json_u64(pv["bound_port"], std::numeric_limits<PortIndex>::max(), n)) {
                        result.error = "invalid promoted parameter binding";
                        return result;
                    }
                    p.bound_port = (PortIndex)n;
                    d.promoted_parameters.push_back(std::move(p));
                }
                const auto& lv = rv["limits"];
                if (!lv.isObject()) {
                    result.error = "invalid sample region limits";
                    return result;
                }
                auto u32 = [&](const char* k, std::uint32_t& v) {
                    if (!json_u64(lv[k], std::numeric_limits<std::uint32_t>::max(), n))
                        return false;
                    v = (std::uint32_t)n;
                    return true;
                };
                auto u64 = [&](const char* k, std::uint64_t& v) {
                    return json_u64(lv[k], std::numeric_limits<std::uint64_t>::max(), v);
                };
                if (!u32("max_member_nodes", d.limits.max_member_nodes) ||
                    !u32("max_internal_connections", d.limits.max_internal_connections) ||
                    !u32("max_input_boundaries", d.limits.max_input_boundaries) ||
                    !u32("max_output_boundaries", d.limits.max_output_boundaries) ||
                    !u32("max_delay_nodes", d.limits.max_delay_nodes) ||
                    !u32("max_promoted_parameters", d.limits.max_promoted_parameters) ||
                    !u32("max_state_bytes", d.limits.max_state_bytes) ||
                    !u64("max_logical_boundary_bytes", d.limits.max_logical_boundary_bytes) ||
                    !u32("max_work_per_frame", d.limits.max_work_per_frame) ||
                    !u64("max_work_per_block", d.limits.max_work_per_block)) {
                    result.error = "invalid sample region limits";
                    return result;
                }
                detail::accumulate_parser_region_shape(graph, d, shape);
                parsed_regions.push_back(std::move(parsed));
            }
            if (has_region_records) {
                shape.connections_total = parsed_connections.size();
                shape.available_bytes = json.size();
                for (const auto& r : parsed_regions) {
                    std::uint64_t region_connections = 0;
                    for (const auto& c : parsed_connections) {
                        const auto touches = [&](NodeId id) {
                            return std::any_of(r.definition.members.begin(),
                                               r.definition.members.end(),
                                               [&](const auto& m) { return m.node == id; });
                        };
                        if (touches(c.source_node) || touches(c.dest_node))
                            ++region_connections;
                    }
                    shape.connections_per_region =
                        std::max(shape.connections_per_region, region_connections);
                }
                if (!prove_sample_region_parser_shape(shape).accepted) {
                    result.error = "sample region parser shape exceeds limits";
                    return result;
                }
            }
        }
    } catch (const std::exception& e) {
        result.error = std::string("preflight failed: ") + e.what();
        return result;
    }

    // From here on, choc field accessors (getInt64 / getString / etc.) throw
    // on type mismatch. Wrap the deserialization body so a malformed
    // .pulpgraph leaves the graph in its cleared state with a clear error.
    try {

    // Pass 1: instantiate every node, build old-id → new-id map.
    std::unordered_map<NodeId, NodeId> id_map;
    std::unordered_map<NodeId, std::shared_ptr<format::ProcessorNodeInstance>> restored_processors;
    if (root.hasObjectMember("processors")) {
        if (!options.processor_registry) {
            result.error = "Processor records require a ProcessorRestoreRegistry";
            return result;
        }
        const auto& records_json = root["processors"];
        if (!records_json.isArray() || records_json.size() > ProcessorRestoreRegistry::max_nodes) {
            result.error = "invalid processors envelope";
            return result;
        }
        std::vector<std::vector<std::uint8_t>> parameter_blobs, custom_blobs;
        std::vector<ProcessorRestoreRecord> records;
        std::size_t decoded_total = 0;
        parameter_blobs.reserve(records_json.size());
        custom_blobs.reserve(records_json.size());
        records.reserve(records_json.size());
        for (uint32_t i = 0; i < records_json.size(); ++i) {
            const auto& p = records_json[i];
            const auto bounded_encoded = [](std::string_view encoded) {
                constexpr std::size_t max_encoded =
                    ((ProcessorRestoreRegistry::max_state_part_bytes + 2) / 3) * 4 + 8;
                return encoded.size() <= max_encoded;
            };
            const auto parameters_text = std::string_view(p["parameters_b64"].getString());
            const auto custom_text = std::string_view(p["custom_b64"].getString());
            if (!bounded_encoded(parameters_text) || !bounded_encoded(custom_text)) {
                result.error = "Processor state encoding exceeds limits";
                return result;
            }
            const auto parameter_size = b64_decoded_size(parameters_text);
            const auto custom_size = b64_decoded_size(custom_text);
            if (!parameter_size || !custom_size) {
                result.error = "invalid Processor state encoding";
                return result;
            }
            if (*parameter_size > ProcessorRestoreRegistry::max_state_part_bytes ||
                *custom_size > ProcessorRestoreRegistry::max_state_part_bytes ||
                *parameter_size > ProcessorRestoreRegistry::max_total_state_bytes - decoded_total ||
                *custom_size > ProcessorRestoreRegistry::max_total_state_bytes - decoded_total -
                                   *parameter_size) {
                result.error = "Processor state exceeds aggregate limit";
                return result;
            }
            auto parameters = b64_decode(parameters_text);
            auto custom = b64_decode(custom_text);
            if (!parameters || !custom) {
                result.error = "invalid Processor state encoding";
                return result;
            }
            if (parameters->size() > ProcessorRestoreRegistry::max_state_part_bytes ||
                custom->size() > ProcessorRestoreRegistry::max_state_part_bytes ||
                parameters->size() >
                    ProcessorRestoreRegistry::max_total_state_bytes - decoded_total) {
                result.error = "Processor state exceeds limits";
                return result;
            }
            decoded_total += parameters->size();
            if (custom->size() > ProcessorRestoreRegistry::max_total_state_bytes - decoded_total) {
                result.error = "Processor state exceeds limits";
                return result;
            }
            decoded_total += custom->size();
            const auto node_id_value = p["node_id"].getInt64();
            const auto type_version_value = p["type_version"].getInt64();
            const auto envelope_version_value = p["state_schema_version"].getInt64();
            const auto input_channels_value = p["input_channels"].getInt64();
            const auto output_channels_value = p["output_channels"].getInt64();
            if (node_id_value <= 0 ||
                static_cast<std::uint64_t>(node_id_value) >
                    std::numeric_limits<std::uint32_t>::max() ||
                type_version_value <= 0 || type_version_value > std::numeric_limits<int>::max() ||
                envelope_version_value <= 0 ||
                envelope_version_value > std::numeric_limits<int>::max() ||
                input_channels_value < 0 ||
                input_channels_value > std::numeric_limits<int>::max() ||
                output_channels_value < 0 ||
                output_channels_value > std::numeric_limits<int>::max()) {
                result.error = "invalid Processor record range";
                return result;
            }
            parameter_blobs.push_back(std::move(*parameters));
            custom_blobs.push_back(std::move(*custom));
            records.push_back(
                {static_cast<std::uint32_t>(node_id_value),
                 {std::string(p["type_id"].getString()), static_cast<int>(type_version_value)},
                 static_cast<int>(envelope_version_value),
                 static_cast<int>(input_channels_value),
                 static_cast<int>(output_channels_value),
                 parameter_blobs.back(),
                 custom_blobs.back()});
        }
        const auto restored =
            options.processor_registry->restore_batch(records, options.processor_allowlist);
        if (!restored.ok()) {
            result.error = std::string("Processor restore failed: ") +
                           processor_restore_code_name(restored.code);
            return result;
        }
        for (auto& item : restored.processors)
            restored_processors.emplace(item.node_id, std::move(item.instance));
    }
    std::unordered_set<NodeId> seen_persisted_node_ids;
    std::unordered_map<NodeId, std::pair<NodeId, std::vector<uint8_t>>> deferred_state;
    std::unordered_set<NodeId> unresolved_plugin_nodes;

    if (root.hasObjectMember("nodes")) {
        const auto& nodes = root["nodes"];
        for (uint32_t i = 0; i < nodes.size(); ++i) {
            const auto& nv = nodes[i];
            const auto old_id_value = nv["id"].getInt64();
            const NodeId old_id = (NodeId)old_id_value;
            if (old_id_value <= 0 ||
                static_cast<std::uint64_t>(old_id_value) > std::numeric_limits<NodeId>::max() ||
                !seen_persisted_node_ids.insert(old_id).second) {
                result.error = "invalid or duplicate node id in sample region graph";
                return result;
            }
            const std::string name(nv["name"].getString());
            const std::string type_s(nv["type"].getString());
            const float gain = nv.hasObjectMember("gain") ? (float)get_double(nv["gain"]) : 1.0f;
            NodeId new_id = 0;
            if (type_s == "processor") {
                if (!root.hasObjectMember("processors")) {
                    result.error =
                        "Processor nodes are runtime-owned and unsupported by .pulpgraph v1";
                    return result;
                }
                const auto restored = restored_processors.find(old_id);
                if (restored == restored_processors.end()) {
                    result.error = "Processor node missing restore record";
                    return result;
                }
                auto instance = std::move(restored->second);
                restored_processors.erase(restored);
                const auto& descriptor = instance->descriptor();
                const auto persisted_inputs = nv["num_input_ports"].getInt64();
                const auto persisted_outputs = nv["num_output_ports"].getInt64();
                if (persisted_inputs != descriptor.default_input_channels() ||
                    persisted_outputs != descriptor.default_output_channels()) {
                    result.error = "Processor node port shape does not match restore descriptor";
                    return result;
                }
                new_id = graph.add_processor_node(std::move(instance), name);
                if (new_id == 0) {
                    result.error = "failed to add restored Processor node";
                    return result;
                }
                id_map[old_id] = new_id;
                graph.set_node_gain(new_id, gain);
                if (nv.hasObjectMember("layout")) {
                    const auto& pos = nv["layout"];
                    result.editor_layout[new_id] = {(float)get_double(pos["x"]),
                                                    (float)get_double(pos["y"])};
                }
                continue;
            }
            NodeType t = NodeType::Custom;
            const bool known_type = parse_type(type_s, t);
            const int in_ch  = (int)nv["num_input_ports"].getInt64();
            const int out_ch = (int)nv["num_output_ports"].getInt64();
            std::string node_shape_error;
            if (!validate_generated_node_shape(
                    t, type_s, in_ch, out_ch, node_shape_error)) {
                result.error = node_shape_error;
                return result;
            }

            switch (t) {
                case NodeType::AudioInput:  new_id = graph.add_input_node(out_ch, name); break;
                case NodeType::AudioOutput: new_id = graph.add_output_node(in_ch, name); break;
                case NodeType::Gain:        new_id = graph.add_gain_node(name); break;
                case NodeType::MidiInput:   new_id = graph.add_midi_input_node(name); break;
                case NodeType::MidiOutput:  new_id = graph.add_midi_output_node(name); break;
                case NodeType::Custom: {
                    std::string type_id = type_s;
                    int version = 1;
                    if (known_type && nv.hasObjectMember("custom")) {
                        const auto& cv = nv["custom"];
                        if (cv.hasObjectMember("type_id")) {
                            type_id = cv["type_id"].getString();
                        }
                        if (cv.hasObjectMember("version")) {
                            version = (int)cv["version"].getInt64();
                        }
                    }
                    // The Control index input is built in: register it on
                    // demand, exactly as add_control_index_input_node() does,
                    // so a restored graph prepares without a caller step.
                    if (type_id == kControlIndexInputTypeId &&
                        version == kControlIndexInputVersion &&
                        graph.custom_node_type(type_id, version) == nullptr)
                        (void)graph.register_custom_node_type(make_control_index_input_type());
                    const auto* registered = graph.custom_node_type(type_id, version);
                    const bool has_custom = nv.hasObjectMember("custom");
                    const auto parse_ports = [&](const char* key, int count, bool output,
                                                 std::vector<CustomNodePortDescriptor>& ports) {
                        if (count < 0 || count > static_cast<int>(kMaxSerializedConnections))
                            return false;
                        const auto& cv = nv["custom"];
                        const bool present = has_custom && cv.hasObjectMember(key);
                        if (present && (!cv[key].isArray() ||
                                        cv[key].size() != static_cast<std::uint32_t>(count)))
                            return false;
                        ports.reserve(static_cast<std::size_t>(count));
                        for (int i = 0; i < count; ++i) {
                            CustomNodePortDescriptor port;
                            if (present) {
                                const auto& item = cv[key][static_cast<std::uint32_t>(i)];
                                std::uint64_t kind = 0;
                                if (!item.isObject() || !item.hasObjectMember("name") ||
                                    !item["name"].isString() ||
                                    !json_u64(item["kind"],
                                              static_cast<unsigned>(PortKind::Spectral), kind))
                                    return false;
                                port.name = std::string(item["name"].getString());
                                port.kind = static_cast<PortKind>(kind);
                            } else {
                                port.name =
                                    i == 0 ? "audio"
                                           : std::string(output ? "out" : "in") + std::to_string(i);
                            }
                            if (port.name.empty() || port.name.size() > 128 ||
                                std::any_of(ports.begin(), ports.end(), [&](const auto& prior) {
                                    return prior.name == port.name;
                                }))
                                return false;
                            ports.push_back(std::move(port));
                        }
                        return true;
                    };
                    std::vector<CustomNodePortDescriptor> input_ports;
                    std::vector<CustomNodePortDescriptor> output_ports;
                    const bool has_input_descriptors =
                        has_custom && nv["custom"].hasObjectMember("input_ports");
                    const bool has_output_descriptors =
                        has_custom && nv["custom"].hasObjectMember("output_ports");
                    has_typed_custom_port_payload |= has_input_descriptors;
                    if (has_input_descriptors && source_format_version < 5) {
                        result.error = "typed Custom ports require graph format v5";
                        return result;
                    }
                    if (has_input_descriptors != has_output_descriptors ||
                        !parse_ports("input_ports", in_ch, false, input_ports) ||
                        !parse_ports("output_ports", out_ch, true, output_ports)) {
                        result.error = "invalid Custom port descriptors";
                        return result;
                    }
                    if (registered && (has_input_descriptors || has_output_descriptors)) {
                        GraphNode persisted_shape{};
                        persisted_shape.num_input_ports = in_ch;
                        persisted_shape.num_output_ports = out_ch;
                        for (const auto& port : input_ports)
                            persisted_shape.input_ports.push_back(
                                {0, 0, false, port.name, port.kind});
                        for (const auto& port : output_ports)
                            persisted_shape.output_ports.push_back(
                                {0, 0, true, port.name, port.kind});
                        if (!registered->matches_ports(persisted_shape)) {
                            result.error =
                                "registered Custom port descriptors differ from persisted graph";
                            return result;
                        }
                    }
                    if (registered && registered->num_input_ports == in_ch &&
                        registered->num_output_ports == out_ch) {
                        new_id = graph.add_custom_node(type_id, version, name);
                    } else {
                        result.missing_custom_node_types.push_back(
                            custom_type_label(type_id, version));
                        new_id = graph.add_unresolved_custom_node(type_id, version, in_ch, out_ch,
                                                                  name, input_ports, output_ports);
                    }
                    // Restore opaque custom-node state. Applied to a resolved
                    // node's instance on its next prepare(); for an unresolved
                    // node the blob is preserved so a re-save keeps it.
                    if (new_id != 0 && nv.hasObjectMember("custom")) {
                        const auto& cv2 = nv["custom"];
                        if (cv2.hasObjectMember("state_b64")) {
                            auto blob = b64_decode(cv2["state_b64"].getString());
                            if (!blob) {
                                result.error = "invalid custom state_b64";
                                return result;
                            }
                            graph.set_custom_node_state(new_id, *blob);
                        }
                    }
                    break;
                }
                case NodeType::Plugin: {
                    if (!nv.hasObjectMember("plugin")) {
                        result.missing_plugins.push_back(name + " (no plugin info)");
                        break;
                    }
                    const auto& pv = nv["plugin"];
                    PluginInfo info;
                    info.name         = pv["name"].getString();
                    info.manufacturer = pv["manufacturer"].getString();
                    info.version      = pv["version"].getString();
                    info.unique_id    = pv["unique_id"].getString();
                    info.path         = pv["last_path"].getString();
                    info.format       = parse_format(pv["format"].getString());
                    info.num_inputs   = in_ch;
                    info.num_outputs  = out_ch;
                    std::string unresolved_reason;
                    std::unique_ptr<PluginSlot> slot;
                    if (const auto load_path = plugin_resolver.resolve(info, unresolved_reason)) {
                        PluginInfo load_info = info;
                        load_info.path = *load_path;
                        slot = PluginSlot::load(load_info);
                        if (!slot) {
                            unresolved_reason = "the plugin failed to load";
                        } else if (!info.unique_id.empty() && !slot->info().unique_id.empty() &&
                                   slot->info().unique_id != info.unique_id) {
                            // The bundle at that path holds some other plugin
                            // (replaced in place, or a stale path reused by a
                            // different install); it is not this node.
                            unresolved_reason = "the bundle at last_path holds plugin '" +
                                                slot->info().unique_id + "', not '" +
                                                info.unique_id + "'";
                            runtime::log_warn("GraphSerializer: refusing '{}' for node '{}': {}",
                                              *load_path, name, unresolved_reason);
                            slot.reset();
                        }
                    }
                    if (!slot) {
                        result.missing_plugins.push_back(std::string(format_str(info.format)) +
                                                         ":" + info.unique_id + " (" +
                                                         unresolved_reason + ")");
                        // Still create a placeholder Plugin node with no slot so
                        // connection IDs remain stable.
                        new_id = graph.add_unresolved_plugin_node(info, in_ch, out_ch, name);
                        if (new_id != 0)
                            unresolved_plugin_nodes.insert(new_id);
                    } else {
                        if (pv.hasObjectMember("state_b64")) {
                            auto blob = b64_decode(pv["state_b64"].getString());
                            if (!blob) {
                                result.error = "invalid plugin state_b64";
                                return result;
                            }
                            if (!blob->empty() && !slot->restore_state(*blob)) {
                                result.state_restore_failures.push_back(
                                    std::string(format_str(info.format)) + ":" + info.unique_id);
                            }
                        }
                        new_id = graph.add_plugin_node(std::move(slot), in_ch, out_ch, name);
                    }
                    break;
                }
            }
            if (new_id == 0) {
                result.unrestored_node_ids.push_back(old_id);
            } else {
                id_map[old_id] = new_id;
                graph.set_node_gain(new_id, gain);
                if (nv.hasObjectMember("layout")) {
                    const auto& pos = nv["layout"];
                    result.editor_layout[new_id] = {
                        (float)get_double(pos["x"]),
                        (float)get_double(pos["y"])
                    };
                }
            }
        }
    }
    if (!restored_processors.empty()) {
        result.error = "Processor restore record has no matching processor node";
        return result;
    }

    // v3 region-bearing graphs are installed as one locked authoring mutation.
    if (!parsed_regions.empty() || has_typed_custom_port_payload ||
        std::any_of(parsed_connections.begin(), parsed_connections.end(),
                    [](const auto& edge) { return edge.control_signal; })) {
        std::vector<SampleRegionDefinition> definitions;
        definitions.reserve(parsed_regions.size());
        const auto remap_node = [&](const char* context, NodeId persisted, NodeId& remapped) {
            const auto it = id_map.find(persisted);
            if (it == id_map.end()) {
                result.error = std::string("sample region ") + context +
                               " references missing node " + std::to_string(persisted);
                return false;
            }
            remapped = it->second;
            return true;
        };
        for (auto& parsed : parsed_regions) {
            auto definition = std::move(parsed.definition);
            for (auto& member : definition.members) {
                if (!remap_node("member", member.node, member.node)) {
                    return result;
                }
            }
            for (auto& id : definition.input_boundaries) {
                if (!remap_node("input boundary", id, id)) {
                    return result;
                }
            }
            for (auto& id : definition.output_boundaries) {
                if (!remap_node("output boundary", id, id)) {
                    return result;
                }
            }
            for (auto& parameter : definition.promoted_parameters) {
                if (!remap_node("promoted parameter", parameter.bound_node_id,
                                parameter.bound_node_id)) {
                    return result;
                }
            }
            definitions.push_back(std::move(definition));
        }
        if (!detail::validate_persisted_region_metadata(graph, definitions, result.error)) {
            return result;
        }
        std::sort(definitions.begin(), definitions.end(),
                  [](const auto& a, const auto& b) { return a.region_id < b.region_id; });
        for (auto& definition : definitions) {
            std::sort(definition.members.begin(), definition.members.end(),
                      [](const auto& a, const auto& b) { return a.node < b.node; });
            std::sort(definition.promoted_parameters.begin(), definition.promoted_parameters.end(),
                      [](const auto& a, const auto& b) { return a.param_id < b.param_id; });
            const auto boundary_index = [&](NodeId id) {
                const auto it = std::find_if(definition.members.begin(), definition.members.end(),
                                             [&](const auto& m) { return m.node == id; });
                return it == definition.members.end() ? std::numeric_limits<std::uint32_t>::max()
                                                      : it->config.boundary_index_or_parameter_id;
            };
            std::sort(definition.input_boundaries.begin(), definition.input_boundaries.end(),
                      [&](NodeId a, NodeId b) { return boundary_index(a) < boundary_index(b); });
            std::sort(definition.output_boundaries.begin(), definition.output_boundaries.end(),
                      [&](NodeId a, NodeId b) { return boundary_index(a) < boundary_index(b); });
        }
        for (auto& connection : parsed_connections) {
            if (!remap_node("connection source", connection.source_node, connection.source_node) ||
                !remap_node("connection destination", connection.dest_node, connection.dest_node)) {
                return result;
            }
        }
        std::string connection_error;
        std::unordered_map<NodeId, SampleRegionId> region_members;
        for (const auto& definition : definitions) {
            for (const auto& member : definition.members) {
                if (!region_members.emplace(member.node, definition.region_id).second) {
                    connection_error = "sample region member belongs to multiple regions";
                    break;
                }
            }
            if (!connection_error.empty())
                break;
        }
        if (connection_error.empty() &&
            !detail::validate_persisted_region_topology(graph, definitions, parsed_connections,
                                                        connection_error)) {
            result.error = std::move(connection_error);
            return result;
        }
        const auto node_region = [&](NodeId id) {
            const auto found = region_members.find(id);
            return found == region_members.end() ? SampleRegionId{} : found->second;
        };
        const auto region_internal = [&](const Connection& connection) {
            const auto source_region = node_region(connection.source_node);
            return source_region != 0 && source_region == node_region(connection.dest_node);
        };

        // Region-internal cycles are legal when a UnitDelay makes them sample-causal,
        // so validate their structural subset manually and stage them before replaying
        // every exterior edge through the normal public connection API.
        {
            SignalGraph::GraphMutationLock lock(graph);
            graph.connections_.clear();
            graph.connection_identities_.clear();
            for (const auto& connection : parsed_connections) {
                const unsigned lane_count =
                    static_cast<unsigned>(connection.midi) +
                    static_cast<unsigned>(connection.automation) +
                    static_cast<unsigned>(connection.audio_rate_modulation) +
                    static_cast<unsigned>(connection.sidechain);
                if (lane_count > 1 || (connection.feedback && lane_count != 0)) {
                    connection_error = "connection has conflicting lane flags";
                    break;
                }
                if ((connection.automation || connection.audio_rate_modulation) &&
                    (connection.automation_mix != AutomationMix::Replace &&
                     connection.automation_mix != AutomationMix::Add)) {
                    connection_error = "connection has an invalid automation mix";
                    break;
                }
                if (!region_internal(connection)) {
                    if ((node_region(connection.source_node) != 0 ||
                         node_region(connection.dest_node) != 0) &&
                        (lane_count != 0 || connection.feedback || connection.control_signal)) {
                        connection_error = "sample region boundary connection is not plain audio";
                        break;
                    }
                    continue;
                }
                const auto* source = graph.node(connection.source_node);
                const auto* destination = graph.node(connection.dest_node);
                if (source == nullptr || destination == nullptr || lane_count != 0 ||
                    connection.feedback || connection.control_signal ||
                    source->num_output_ports <= 0 || destination->num_input_ports <= 0 ||
                    connection.source_port >= static_cast<PortIndex>(source->num_output_ports) ||
                    connection.dest_port >= static_cast<PortIndex>(destination->num_input_ports)) {
                    connection_error = "sample region connection has an invalid shape";
                    break;
                }
                if (std::find(graph.connections_.begin(), graph.connections_.end(), connection) !=
                    graph.connections_.end()) {
                    connection_error = "sample region connection is duplicated";
                    break;
                }
                graph.append_connection_locked_(connection);
            }
        }

        std::vector<bool> drop_connection(parsed_connections.size(), false);
        for (std::size_t i = 0; i < parsed_connections.size(); ++i) {
            const auto& connection = parsed_connections[i];
            if (!connection_error.empty() || region_internal(connection))
                continue;
            bool accepted = false;
            if (connection.audio_rate_modulation) {
                const auto* source = graph.node(connection.source_node);
                if (connection.control_signal && source != nullptr &&
                    connection.source_port < source->output_ports.size()) {
                    accepted = graph.connect_control_modulation(
                        source->output_ports[connection.source_port], connection.dest_node,
                        connection.automation_param_id, connection.automation_range_lo,
                        connection.automation_range_hi, connection.automation_smoothing_ms,
                        connection.automation_mix);
                } else if (!connection.control_signal) {
                    accepted = graph.connect_audio_rate_modulation(
                        connection.source_node, connection.source_port, connection.dest_node,
                        connection.automation_param_id, connection.automation_range_lo,
                        connection.automation_range_hi, connection.automation_smoothing_ms,
                        connection.automation_mix);
                }
            } else if (connection.automation) {
                accepted = graph.connect_automation(
                    connection.source_node, connection.source_port, connection.dest_node,
                    connection.automation_param_id, connection.automation_range_lo,
                    connection.automation_range_hi, connection.automation_smoothing_ms,
                    connection.automation_mix);
            } else if (connection.sidechain) {
                accepted = graph.connect_sidechain(connection.source_node, connection.source_port,
                                                   connection.dest_node, connection.dest_port);
            } else if (connection.midi) {
                accepted = graph.connect_midi(connection.source_node, connection.dest_node);
            } else if (connection.feedback) {
                accepted = graph.connect_feedback(connection.source_node, connection.source_port,
                                                  connection.dest_node, connection.dest_port);
            } else if (connection.control_signal) {
                const auto* source = graph.node(connection.source_node);
                const auto* destination = graph.node(connection.dest_node);
                if (source != nullptr && destination != nullptr &&
                    connection.source_port < source->output_ports.size() &&
                    connection.dest_port < destination->input_ports.size())
                    accepted = static_cast<bool>(
                        graph.connect(source->output_ports[connection.source_port],
                                      destination->input_ports[connection.dest_port]));
            } else {
                accepted = graph.connect(connection.source_node, connection.source_port,
                                         connection.dest_node, connection.dest_port);
            }
            if (accepted) {
                SignalGraph::GraphMutationLock lock(graph);
                parsed_connections[i] = graph.connections_.back();
            }
            const auto* unresolved_source = graph.node(connection.source_node);
            const bool valid_unresolved_source =
                unresolved_source != nullptr && unresolved_source->num_output_ports > 0 &&
                connection.source_port <
                    static_cast<PortIndex>(unresolved_source->num_output_ports);
            if (!accepted && valid_unresolved_source &&
                (connection.automation || connection.audio_rate_modulation) &&
                unresolved_plugin_nodes.contains(connection.dest_node)) {
                // Match the legacy partial-load contract: an unavailable plugin
                // cannot accept parameter routing, so omit only that edge while
                // preserving its placeholder and the rest of the graph.
                drop_connection[i] = true;
                continue;
            }
            if (!accepted)
                connection_error = "connection failed validation";
        }

        if (connection_error.empty()) {
            SignalGraph::GraphMutationLock lock(graph);
            std::vector<SampleRegionCandidate> resolved_candidates;
            for (const auto& definition : definitions) {
                const bool fully_resolved = std::all_of(
                    definition.members.begin(), definition.members.end(), [&](const auto& member) {
                        return graph.sample_kernel_type(member.type_id, member.version) != nullptr;
                    });
                if (!fully_resolved)
                    continue;
                const auto metadata = graph.sample_region_metadata_proof_locked_(definition, true);
                if (!metadata.accepted) {
                    connection_error = "sample region metadata proof failed: " + metadata.message;
                    break;
                }
                auto candidate = graph.sample_region_candidate_locked_(definition);
                // Persistence has no runtime block size. One frame is the smallest
                // preparable block and preserves every structural and aggregate
                // admission check without inventing a later host block size.
                candidate.max_block_size = 1;
                resolved_candidates.push_back(std::move(candidate));
            }
            if (connection_error.empty() && !resolved_candidates.empty()) {
                const auto proof = pulp::host::prove_sample_regions(resolved_candidates);
                if (!proof.accepted) {
                    connection_error =
                        "sample region proof failed: complete graph admission rejected";
                    if (!proof.region_proof.message.empty())
                        connection_error += ": " + proof.region_proof.message;
                }
            }
            if (connection_error.empty()) {
                for (const auto& node : graph.nodes_) {
                    if (node.type == NodeType::Custom &&
                        graph.sample_kernel_type(node.custom_type_id, node.custom_type_version) !=
                            nullptr &&
                        !region_members.contains(node.id)) {
                        connection_error = "sample region proof failed: scalar kernel is outside "
                                           "every declared region";
                        break;
                    }
                }
            }
        }

        if (connection_error.empty()) {
            SignalGraph::GraphMutationLock lock(graph);
            graph.connections_.clear();
            graph.connection_identities_.clear();
            for (std::size_t i = 0; i < parsed_connections.size(); ++i) {
                if (!drop_connection[i])
                    graph.append_connection_locked_(parsed_connections[i]);
            }
            graph.sample_region_definitions_ = std::move(definitions);
            graph.invalidate_live_locked_();
        }
        if (!connection_error.empty()) {
            result.error = std::move(connection_error);
            return result;
        }
    } else if (root.hasObjectMember("connections")) {
        // Pass 2: replay connections using the id map (legacy v1/v2 path).
        const auto& conns = root["connections"];
        for (uint32_t i = 0; i < conns.size(); ++i) {
            const auto& cv = conns[i];
            NodeId src_old = (NodeId)cv["source_node"].getInt64();
            NodeId dst_old = (NodeId)cv["dest_node"].getInt64();
            auto sit = id_map.find(src_old);
            auto dit = id_map.find(dst_old);
            if (sit == id_map.end() || dit == id_map.end()) continue;
            NodeId src = sit->second;
            NodeId dst = dit->second;
            PortIndex sp = (PortIndex)cv["source_port"].getInt64();
            PortIndex dp = (PortIndex)cv["dest_port"].getInt64();
            const bool fb   = cv["feedback"].getBool();
            const bool md   = cv["midi"].getBool();
            const bool au   = cv["automation"].getBool();
            const bool ar   = cv.hasObjectMember("audio_rate_modulation")
                ? cv["audio_rate_modulation"].getBool()
                : false;
            const bool sc   = cv.hasObjectMember("sidechain")
                ? cv["sidechain"].getBool()
                : false;
            if (ar) {
                graph.connect_audio_rate_modulation(
                    src, sp, dst,
                    (uint32_t)cv["auto_param_id"].getInt64(),
                    (float)get_double(cv["auto_range_lo"]),
                    (float)get_double(cv["auto_range_hi"]),
                    (float)get_double(cv["auto_smoothing"]),
                    (AutomationMix)(uint8_t)cv["auto_mix"].getInt64());
            } else if (au) {
                graph.connect_automation(
                    src, sp, dst,
                    (uint32_t)cv["auto_param_id"].getInt64(),
                    (float)get_double(cv["auto_range_lo"]),
                    (float)get_double(cv["auto_range_hi"]),
                    (float)get_double(cv["auto_smoothing"]),
                    (AutomationMix)(uint8_t)cv["auto_mix"].getInt64());
            } else if (fb) {
                graph.connect_feedback(src, sp, dst, dp);
            } else if (md) {
                graph.connect_midi(src, dst);
            } else if (sc) {
                graph.connect_sidechain(src, sp, dst, dp);
            } else {
                graph.connect(src, sp, dst, dp);
            }
        }
    }

    {
        // Every node this load created takes the authored ID its record
        // restores with, replacing the fresh one minted by the add call.
        SignalGraph::GraphMutationLock lock(graph);
        for (const auto& [persisted, live] : id_map) {
            auto* node = graph.node_mut_locked_(live);
            const auto authored = authored_ids.find(persisted);
            if (node != nullptr && authored != authored_ids.end())
                node->authored_id = AuthoredNodeId(authored->second);
        }
    }
    if (root.hasObjectMember("event_connections")) {
        // Rebound last, once every node carries its authored ID and every
        // connection is restored, so the binding sees the path it requires.
        // Inspection already refused a malformed, dangling or repeated
        // record; what remains is the graph's own binding rule.
        if (g_fail_next_event_rebind.exchange(false, std::memory_order_acq_rel))
            throw std::bad_alloc();
        std::unordered_map<std::string, NodeId> live_by_authored;
        for (const auto& [persisted, live] : id_map)
            if (const auto authored = authored_ids.find(persisted); authored != authored_ids.end())
                live_by_authored.emplace(authored->second, live);
        const auto& events = root["event_connections"];
        for (std::uint32_t i = 0; i < events.size(); ++i) {
            const auto& record = events[i];
            const auto producer = std::string(record["producer"].getString());
            const auto consumer = std::string(record["consumer"].getString());
            const auto label = producer + " -> " + consumer;
            const auto p = live_by_authored.find(producer);
            const auto c = live_by_authored.find(consumer);
            if (p == live_by_authored.end() || c == live_by_authored.end()) {
                result.error =
                    "event_connection_not_restored: an endpoint of " + label + " was not restored";
                return result;
            }
            GraphAuthoredEventEdge edge;
            edge.producer = p->second;
            edge.consumer = c->second;
            edge.source_port = static_cast<std::uint8_t>(record["source_port"].getInt64());
            edge.destination_port =
                static_cast<std::uint8_t>(record["destination_port"].getInt64());
            edge.kind = static_cast<std::uint8_t>(record["kind"].getInt64());
            if (const auto bound = graph.connect_graph_event(edge);
                bound != GraphEventBindingResult::Accepted) {
                result.error = "event_connection_not_restored: " + label + " was refused (" +
                               event_binding_name(bound) + ")";
                return result;
            }
        }
    }
    result.persisted_node_ids = std::move(id_map);

    } catch (const std::exception& e) {
        result.error = std::string("field deserialization failed: ") + e.what();
        return result;
    }

    result.ok = true;
    return result;
}

bool GraphSerializer::copy_custom_node_types(const SignalGraph& from, SignalGraph& to) {
    std::unordered_map<std::string, CustomNodeType> types;
    std::unordered_map<std::string, SampleKernelDescriptor> kernels;
    std::unordered_map<std::string, CustomNodeDiagnosticsDescriptor> diagnostics;
    const SampleRegionParameterBinding* binding = nullptr;
    std::uint32_t proof_block_size = 0;
    {
        SignalGraph::GraphMutationLock lock(from);
        types = from.custom_node_types_;
        kernels = from.sample_kernel_types_;
        diagnostics = from.custom_node_diagnostics_;
        binding = from.sample_region_parameter_binding_;
        proof_block_size = from.sample_region_proof_block_size_;
    }
    SignalGraph::GraphMutationLock lock(to);
    if (to.refuse_spectral_mutation_())
        return false;
    to.custom_node_types_ = std::move(types);
    to.sample_kernel_types_ = std::move(kernels);
    to.custom_node_diagnostics_ = std::move(diagnostics);
    to.sample_region_parameter_binding_ = binding;
    to.sample_region_proof_block_size_ = proof_block_size;
    ++to.custom_registry_generation_;
    return true;
}

} // namespace pulp::host
