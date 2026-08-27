#include "authority_navigation.hpp"

#include "json_parser.hpp"
#include "json_writer.hpp"

#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace pulp::cli::authority {
namespace {

using JsonValue = pulp::cli::pkg::JsonValue;

constexpr const char* kRegistryRelative = "docs/status/authority-navigation.json";
constexpr const char* kSchemaRelative = "docs/status/authority-navigation.schema.json";
constexpr const char* kInstalledRelative = "share/pulp/authority-navigation.json";
constexpr const char* kInstalledSchemaRelative = "share/pulp/authority-navigation.schema.json";
const std::set<std::string> kExpectedAuthorityIds = {
    "agent-capabilities", "dsp-capabilities",   "dsp-survey-admission",
    "forge-catalog",      "live-control",       "offline-cli",
    "offline-mcp",        "sequencer-exposure", "timeline-schema",
};
constexpr const char* kExactLiveQuery =
    "pulp control status --instance <caller-supplied-exact-instance-id> --json";
constexpr const char* kDspAdmissionOwner =
    "danielraffel/pulp-planning:dsp-survey-claims/WORKER-PROMPT.md";
constexpr const char* kDspAdmissionQuery =
    "read dsp-survey-claims/WORKER-PROMPT.md from current danielraffel/pulp-planning main";
constexpr const char* kExpectedSchemaSha256 =
    "ff94b9c5ef5560f65d1cf83b96d0b6104edf1bed203e219689a6c398a48f8572";

std::optional<std::string> normalized_lf(std::string text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\r') {
            normalized.push_back(text[index]);
            continue;
        }
        if (index + 1 >= text.size() || text[index + 1] != '\n')
            return std::nullopt;
        normalized.push_back('\n');
        ++index;
    }
    return normalized;
}

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream out;
    out << stream.rdbuf();
    return stream ? out.str() : std::string{};
}

std::optional<std::string> read_sdk_version(const fs::path& root) {
    std::ifstream stream(root / "version.txt");
    std::string version;
    if (!std::getline(stream, version) || version.empty())
        return std::nullopt;
    return version;
}

std::string sha256_hex(const std::string& text) {
    const auto digest = pulp::runtime::sha256(std::string_view{text});
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : digest)
        out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

fs::path source_root_from(fs::path start) {
    std::error_code ec;
    if (fs::is_regular_file(start, ec))
        start = start.parent_path();
    while (!start.empty()) {
        if (fs::is_regular_file(start / "CMakeLists.txt", ec) &&
            fs::is_regular_file(start / "CLAUDE.md", ec) && fs::is_directory(start / "core", ec) &&
            fs::is_regular_file(start / "tools/cli/pulp_cli.cpp", ec) &&
            fs::is_regular_file(start / "docs/status/cli-commands.yaml", ec))
            return start;
        const auto parent = start.parent_path();
        if (parent == start)
            break;
        start = parent;
    }
    return {};
}

fs::path source_root_from_build_binary(fs::path executable) {
    auto cursor = executable.parent_path();
    for (int depth = 0; depth < 8 && !cursor.empty(); ++depth) {
        const auto cache = cursor / "CMakeCache.txt";
        std::ifstream in(cache);
        std::string line;
        constexpr const char* prefix = "CMAKE_HOME_DIRECTORY:INTERNAL=";
        while (std::getline(in, line)) {
            if (line.rfind(prefix, 0) != 0)
                continue;
            auto source =
                source_root_from(fs::path(line.substr(std::char_traits<char>::length(prefix))));
            if (!source.empty())
                return source;
        }
        const auto parent = cursor.parent_path();
        if (parent == cursor)
            break;
        cursor = parent;
    }
    return {};
}

const JsonValue* required(const JsonValue& object, const std::string& key, JsonValue::Type type,
                          std::string& error) {
    const auto* value = object.get(key);
    if (!value || value->type != type) {
        error = "field '" + key + "' has the wrong type or is missing";
        return nullptr;
    }
    return value;
}

bool exact_keys(const JsonValue& object, std::initializer_list<const char*> expected,
                const std::string& label, std::string& error) {
    std::set<std::string> allowed;
    for (const auto* key : expected)
        allowed.emplace(key);
    std::set<std::string> seen;
    for (const auto& [key, value] : object.obj()) {
        static_cast<void>(value);
        if (!allowed.contains(key)) {
            error = label + " contains unknown field '" + key + "'";
            return false;
        }
        if (!seen.insert(key).second) {
            error = label + " contains duplicate field '" + key + "'";
            return false;
        }
    }
    if (seen.size() != allowed.size()) {
        error = label + " does not contain exactly the required fields";
        return false;
    }
    return true;
}

bool safe_relative(const std::string& value, bool installed) {
    if (value.empty() || value.find('\\') != std::string::npos)
        return false;
    const fs::path path(value);
    if (path.is_absolute())
        return false;
    for (const auto& part : path) {
        if (part.empty() || part == "." || part == "..")
            return false;
    }
    if (!installed)
        return true;
    auto it = path.begin();
    if (it == path.end())
        return false;
    if (*it == "bin")
        return true;
    if (*it != "share")
        return false;
    ++it;
    return it != path.end() && *it == "pulp";
}

bool valid_token(const std::string& value) {
    if (value.empty() || value.size() > 64 || value.front() < 'a' || value.front() > 'z')
        return false;
    bool separator = false;
    for (const char c : value) {
        const bool alpha = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        const bool next_separator = c == '.' || c == '-';
        if (!alpha && !digit && !next_separator)
            return false;
        if (next_separator && separator)
            return false;
        separator = next_separator;
    }
    return !separator;
}

bool trimmed_nonempty(const std::string& value) {
    return !value.empty() && value.front() != ' ' && value.front() != '\t' &&
           value.front() != '\n' && value.front() != '\r' && value.back() != ' ' &&
           value.back() != '\t' && value.back() != '\n' && value.back() != '\r';
}

std::optional<std::string> optional_string(const JsonValue* value, std::string& error,
                                           const std::string& field) {
    if (!value || value->type == JsonValue::Null)
        return std::nullopt;
    if (value->type != JsonValue::String || value->str_val.empty()) {
        error = "field '" + field + "' must be a non-empty string or null";
        return std::nullopt;
    }
    return value->str_val;
}

bool parse_authority(const JsonValue& value, Authority& row, std::string& error) {
    if (value.type != JsonValue::Object) {
        error = "authority row must be an object";
        return false;
    }
    if (!exact_keys(value,
                    {"id", "aliases", "plane", "native_owner", "source_location",
                     "installed_location", "query_or_validator", "coverage_semantics",
                     "absence_semantics", "does_not_prove"},
                    "authority row", error))
        return false;
    const auto* id = required(value, "id", JsonValue::String, error);
    const auto* aliases = required(value, "aliases", JsonValue::Array, error);
    const auto* plane = required(value, "plane", JsonValue::String, error);
    const auto* owner = required(value, "native_owner", JsonValue::String, error);
    const auto* source = required(value, "source_location", JsonValue::String, error);
    const auto* query = required(value, "query_or_validator", JsonValue::Object, error);
    const auto* coverage = required(value, "coverage_semantics", JsonValue::String, error);
    const auto* absence = required(value, "absence_semantics", JsonValue::String, error);
    const auto* disclaimers = required(value, "does_not_prove", JsonValue::Array, error);
    if (!id || !aliases || !plane || !owner || !source || !query || !coverage || !absence ||
        !disclaimers)
        return false;

    row.id = id->str_val;
    row.aliases = aliases->as_string_array();
    if (row.aliases.size() != aliases->arr().size() || row.aliases.empty() ||
        row.aliases.size() > 8) {
        error = "authority aliases must be a non-empty string array";
        return false;
    }
    row.plane = plane->str_val;
    row.native_owner = owner->str_val;
    row.source_location = source->str_val;
    row.coverage_semantics = coverage->str_val;
    row.absence_semantics = absence->str_val;
    row.does_not_prove = disclaimers->as_string_array();
    if (row.does_not_prove.size() != disclaimers->arr().size() || row.does_not_prove.empty()) {
        error = "does_not_prove must be a non-empty string array";
        return false;
    }
    const std::set<std::string> planes = {"design_time_static", "dsp_static",
                                          "live_control",       "offline_command",
                                          "release_evidence",   "timeline_static"};
    const std::set<std::string> absences = {"not_advertised_by_the_queried_mcp_server",
                                            "not_present_in_covered_forge_bake_catalog_headers",
                                            "not_present_in_joined_forge_catalog",
                                            "not_registered_as_a_top_level_cli_command",
                                            "not_registered_in_builtin_timeline_schema_registry",
                                            "requires_exact_live_instance",
                                            "unknown",
                                            "unknown_outside_reverified_rows"};
    if (!valid_token(row.id) || !planes.contains(row.plane) ||
        !trimmed_nonempty(row.native_owner) || row.native_owner.size() > 512 ||
        !trimmed_nonempty(row.coverage_semantics) || row.coverage_semantics.size() > 1024 ||
        !absences.contains(row.absence_semantics) || !safe_relative(row.source_location, false)) {
        error = "authority row contains an empty field or unsafe source location";
        return false;
    }

    const auto* installed_value = value.get("installed_location");
    if (!installed_value) {
        error = "field 'installed_location' is missing";
        return false;
    }
    row.installed_location = optional_string(installed_value, error, "installed_location");
    if (!error.empty())
        return false;
    if (row.installed_location && !safe_relative(*row.installed_location, true)) {
        error = "installed_location is not a safe prefix-relative path";
        return false;
    }
    if (row.source_location.size() > 512 ||
        (row.installed_location && row.installed_location->size() > 512)) {
        error = "authority location exceeds the v1 bound";
        return false;
    }

    const auto* source_query = required(*query, "source", JsonValue::String, error);
    const auto* installed_query = query->get("installed");
    if (!source_query || !installed_query) {
        if (error.empty())
            error = "query_or_validator.installed is missing";
        return false;
    }
    if (!exact_keys(*query, {"source", "installed"}, "query_or_validator", error))
        return false;
    row.query_or_validator.source = source_query->str_val;
    row.query_or_validator.installed =
        optional_string(installed_query, error, "query_or_validator.installed");
    if (!error.empty())
        return false;
    if (!trimmed_nonempty(row.query_or_validator.source)) {
        error = "query_or_validator.source must be a trimmed non-empty string";
        return false;
    }
    if (row.query_or_validator.source.size() > 512 ||
        (row.query_or_validator.installed && row.query_or_validator.installed->size() > 512)) {
        error = "authority query exceeds the v1 bound";
        return false;
    }
    if (row.installed_location && !row.query_or_validator.installed) {
        error = "installed authority artifact has no installed native query";
        return false;
    }
    if (row.query_or_validator.source.find("pulp authority") != std::string::npos ||
        (row.query_or_validator.installed &&
         row.query_or_validator.installed->find("pulp authority") != std::string::npos)) {
        error = "authority query cannot route back to the navigator";
        return false;
    }
    if (row.plane == "live_control") {
        const auto& installed = row.query_or_validator.installed;
        if (row.absence_semantics != "requires_exact_live_instance" || !installed ||
            row.query_or_validator.source != kExactLiveQuery || *installed != kExactLiveQuery) {
            error = "live-control route must require a caller-supplied exact instance";
            return false;
        }
    }
    if (row.id == "dsp-survey-admission" &&
        (row.plane != "release_evidence" || row.native_owner != kDspAdmissionOwner ||
         row.source_location != "planning" || row.installed_location ||
         row.query_or_validator.source != kDspAdmissionQuery || row.query_or_validator.installed)) {
        error = "dsp-survey-admission must remain source-only and route to current planning main";
        return false;
    }
    for (const auto& alias : row.aliases) {
        if (!valid_token(alias)) {
            error = "authority alias is not canonical lowercase ASCII";
            return false;
        }
    }
    if (row.does_not_prove.size() > 8 ||
        std::set<std::string>(row.does_not_prove.begin(), row.does_not_prove.end()).size() !=
            row.does_not_prove.size()) {
        error = "does_not_prove exceeds the v1 bound or contains duplicates";
        return false;
    }
    for (const auto& disclaimer : row.does_not_prove) {
        if (!trimmed_nonempty(disclaimer) || disclaimer.size() > 1024) {
            error = "does_not_prove entries must be trimmed non-empty strings";
            return false;
        }
    }
    return true;
}

std::string context_name(Context context) {
    return context == Context::source ? "source" : "installed";
}

struct ContextRoute {
    std::string status;
    std::optional<fs::path> artifact;
    std::optional<std::string> query;
};

ContextRoute context_route(const Registry& registry, const Authority& row) {
    if (row.plane == "live_control") {
        const auto query = registry.resolved.context == Context::source
                               ? std::optional<std::string>{row.query_or_validator.source}
                               : row.query_or_validator.installed;
        return {"requires_exact_live_instance", std::nullopt, query};
    }
    if (registry.resolved.context == Context::source) {
        if (row.id == "dsp-survey-admission")
            return {"requires_current_pulp_planning_main", std::nullopt,
                    row.query_or_validator.source};
        return {fs::exists(registry.resolved.context_root / row.source_location)
                    ? "available"
                    : "missing_native_artifact",
                registry.resolved.context_root / row.source_location,
                row.query_or_validator.source};
    }
    if (row.installed_location) {
        const auto artifact = registry.resolved.context_root / *row.installed_location;
        return {fs::is_regular_file(artifact) ? "available" : "missing_native_artifact", artifact,
                row.query_or_validator.installed};
    }
    return {"unavailable_in_installed_context", std::nullopt, std::nullopt};
}

void append_string_array(std::ostringstream& out, const std::vector<std::string>& values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i)
            out << ",";
        out << json_string(values[i]);
    }
    out << "]";
}

void append_row_json(std::ostringstream& out, const Registry& registry, const Authority& row,
                     const std::string& matched_token) {
    const auto route = context_route(registry, row);
    out << "{\"id\":" << json_string(row.id) << ",\"matched_token\":" << json_string(matched_token)
        << ",\"aliases\":";
    append_string_array(out, row.aliases);
    out << ",\"plane\":" << json_string(row.plane)
        << ",\"native_owner\":" << json_string(row.native_owner)
        << ",\"source_location\":" << json_string(row.source_location)
        << ",\"installed_location\":";
    if (row.installed_location)
        out << json_string(*row.installed_location);
    else
        out << "null";
    out << ",\"query_or_validator\":";
    if (route.query)
        out << json_string(*route.query);
    else
        out << "null";
    out << ",\"context_status\":" << json_string(route.status) << ",\"context_artifact\":";
    if (route.artifact)
        out << json_string(route.artifact->generic_string());
    else
        out << "null";
    out << ",\"coverage_semantics\":" << json_string(row.coverage_semantics)
        << ",\"absence_semantics\":" << json_string(row.absence_semantics)
        << ",\"does_not_prove\":";
    append_string_array(out, row.does_not_prove);
    out << "}";
}

std::string receipt_prefix(const Registry& registry) {
    std::ostringstream out;
    out << "\"schema\":\"pulp.authority-navigation.query.v1\",\"registry_revision\":"
        << registry.revision
        << ",\"registry_context\":" << json_string(context_name(registry.resolved.context))
        << ",\"resolution_source\":" << json_string(registry.resolved.resolution_source)
        << ",\"resolved_root\":" << json_string(registry.resolved.context_root.generic_string())
        << ",\"registry_path\":" << json_string(registry.resolved.path.generic_string())
        << ",\"registry_sha256\":" << json_string(registry.sha256) << ",\"selected_sdk_version\":";
    if (registry.resolved.sdk_version)
        out << json_string(*registry.resolved.sdk_version);
    else
        out << "null";
    return out.str();
}

} // namespace

std::optional<ResolvedRegistry> resolve_registry(const fs::path& cwd, const fs::path& executable,
                                                 const fs::path& selected_sdk,
                                                 const std::string& selected_sdk_version,
                                                 const std::string& selected_sdk_source) {
    if (const auto root = source_root_from(cwd); !root.empty()) {
        return ResolvedRegistry{root / kRegistryRelative, root / kSchemaRelative, root,
                                Context::source,          "source_checkout",      std::nullopt};
    }
    if (!selected_sdk.empty()) {
        return ResolvedRegistry{selected_sdk / kInstalledRelative,
                                selected_sdk / kInstalledSchemaRelative,
                                selected_sdk,
                                Context::installed,
                                selected_sdk_source.empty() ? "selected_sdk" : selected_sdk_source,
                                selected_sdk_version.empty()
                                    ? std::nullopt
                                    : std::optional<std::string>{selected_sdk_version}};
    }
    if (const auto root = source_root_from_build_binary(executable); !root.empty()) {
        return ResolvedRegistry{root / kRegistryRelative, root / kSchemaRelative, root,
                                Context::source,          "source_build",         std::nullopt};
    }
    const auto prefix = executable.parent_path().parent_path();
    const auto candidate = prefix / kInstalledRelative;
    if (!prefix.empty() && fs::is_regular_file(candidate))
        return ResolvedRegistry{candidate,      prefix / kInstalledSchemaRelative,
                                prefix,         Context::installed,
                                "adjacent_sdk", read_sdk_version(prefix)};
    return std::nullopt;
}

LoadResult load_registry(const ResolvedRegistry& resolved) {
    if (!fs::is_regular_file(resolved.schema_path))
        return {{}, "registry schema is missing: " + resolved.schema_path.string()};
    std::error_code size_error;
    const auto schema_size = fs::file_size(resolved.schema_path, size_error);
    if (size_error || schema_size > 128 * 1024)
        return {{}, "registry schema exceeds the v1 size bound"};
    const auto raw_schema = read_file(resolved.schema_path);
    const auto canonical_schema = normalized_lf(raw_schema);
    if (!canonical_schema || sha256_hex(*canonical_schema) != kExpectedSchemaSha256)
        return {{}, "registry schema bytes do not match the canonical v1 schema"};
    try {
        static_cast<void>(choc::json::parse(raw_schema));
    } catch (const std::exception& exc) {
        return {{}, "registry schema is not valid JSON: " + std::string(exc.what())};
    }
    pulp::cli::pkg::JsonParser schema_parser{raw_schema};
    auto schema_root = schema_parser.parse();
    schema_parser.skip_ws();
    const auto* schema_id = schema_root.get("$id");
    if (schema_parser.pos != raw_schema.size() || schema_root.type != JsonValue::Object ||
        !schema_id || schema_id->type != JsonValue::String ||
        schema_id->str_val != "https://pulp.audio/schemas/authority-navigation.schema.json")
        return {{}, "registry schema has the wrong identity"};
    size_error.clear();
    if (!fs::is_regular_file(resolved.path))
        return {{}, "registry is missing or exceeds the v1 size bound: " + resolved.path.string()};
    const auto registry_size = fs::file_size(resolved.path, size_error);
    if (size_error || registry_size > 256 * 1024)
        return {{}, "registry is missing or exceeds the v1 size bound: " + resolved.path.string()};
    const auto raw = read_file(resolved.path);
    if (raw.empty())
        return {{}, "registry is missing or empty: " + resolved.path.string()};

    try {
        static_cast<void>(choc::json::parse(raw));
    } catch (const std::exception& exc) {
        return {{}, "registry is not valid JSON: " + std::string(exc.what())};
    }

    pulp::cli::pkg::JsonParser parser{raw};
    auto root = parser.parse();
    parser.skip_ws();
    if (parser.pos != raw.size() || root.type != JsonValue::Object)
        return {{}, "registry is not one complete JSON object"};

    std::string error;
    const auto* schema_ref = required(root, "$schema", JsonValue::String, error);
    const auto* schema = required(root, "schema", JsonValue::String, error);
    const auto* revision = required(root, "registry_revision", JsonValue::Number, error);
    const auto* rows = required(root, "authorities", JsonValue::Array, error);
    if (!schema_ref || !schema || !revision || !rows)
        return {{}, error};
    if (!exact_keys(root, {"$schema", "schema", "registry_revision", "authorities"},
                    "registry root", error))
        return {{}, error};
    if (schema_ref->str_val != "authority-navigation.schema.json" ||
        schema->str_val != "pulp.authority-navigation.v1" || revision->num_val < 1 ||
        revision->num_val > static_cast<double>(std::numeric_limits<int>::max()) ||
        revision->num_val != static_cast<double>(static_cast<int>(revision->num_val)))
        return {{}, "unsupported authority-navigation schema or revision"};
    if (rows->arr().empty() || rows->arr().size() > 32)
        return {{}, "authority count is outside the bounded v1 range"};

    Registry registry;
    registry.revision = revision->as_int();
    registry.sha256 = sha256_hex(raw);
    registry.resolved = resolved;
    std::set<std::string> tokens;
    std::string prior_id;
    for (const auto& value : rows->arr()) {
        Authority row;
        if (!parse_authority(value, row, error))
            return {{}, error};
        if (resolved.context == Context::source &&
            !fs::exists(resolved.context_root / row.source_location))
            return {{}, "native source authority is missing: " + row.source_location};
        if (!prior_id.empty() && row.id <= prior_id)
            return {{}, "authority ids are not strictly bytewise sorted"};
        prior_id = row.id;
        if (!std::is_sorted(row.aliases.begin(), row.aliases.end()))
            return {{}, "aliases are not bytewise sorted for " + row.id};
        for (const auto& token : row.aliases) {
            if (token == row.id || !tokens.insert(token).second)
                return {{}, "duplicate authority id or alias: " + token};
        }
        if (!tokens.insert(row.id).second)
            return {{}, "duplicate authority id or alias: " + row.id};
        registry.authorities.push_back(std::move(row));
    }
    if (registry.authorities.empty())
        return {{}, "registry contains no authorities"};
    std::set<std::string> authority_ids;
    for (const auto& row : registry.authorities)
        authority_ids.insert(row.id);
    if (authority_ids != kExpectedAuthorityIds)
        return {{}, "registry does not contain exactly the finite v1 authority ids"};
    return {std::move(registry), {}};
}

const Authority* find(const Registry& registry, const std::string& token) {
    for (const auto& row : registry.authorities) {
        if (row.id == token ||
            std::find(row.aliases.begin(), row.aliases.end(), token) != row.aliases.end())
            return &row;
    }
    return nullptr;
}

std::string render_list(const Registry& registry, bool json) {
    std::ostringstream out;
    if (json) {
        out << "{" << receipt_prefix(registry) << ",\"authorities\":[";
        for (std::size_t i = 0; i < registry.authorities.size(); ++i) {
            if (i)
                out << ",";
            append_row_json(out, registry, registry.authorities[i], registry.authorities[i].id);
        }
        out << "]}\n";
        return out.str();
    }
    out << "Authority routes (" << context_name(registry.resolved.context) << ")\n"
        << "Registry: " << registry.resolved.path << "\n"
        << "SHA-256: " << registry.sha256 << "\n";
    for (const auto& row : registry.authorities) {
        const auto route = context_route(registry, row);
        out << "  " << row.id << " [" << row.plane << "] — " << route.status << "\n";
    }
    out << "Use `pulp authority query <id-or-alias>` for the native route and limits.\n";
    return out.str();
}

std::optional<std::string> render_query(const Registry& registry, const std::string& token,
                                        bool json) {
    const auto* row = find(registry, token);
    if (!row)
        return std::nullopt;
    const auto route = context_route(registry, *row);
    std::ostringstream out;
    if (json) {
        out << "{" << receipt_prefix(registry) << ",\"authority\":";
        append_row_json(out, registry, *row, token);
        out << "}\n";
        return out.str();
    }
    out << "Authority: " << row->id << "\n"
        << "Matched token: " << token << "\n"
        << "Plane: " << row->plane << "\n"
        << "Registry context: " << context_name(registry.resolved.context) << "\n"
        << "Registry: " << registry.resolved.path << "\n"
        << "Registry SHA-256: " << registry.sha256 << "\n"
        << "Native owner: " << row->native_owner << "\n"
        << "Context status: " << route.status << "\n";
    if (route.artifact)
        out << "Context artifact: " << *route.artifact << "\n";
    if (route.query)
        out << "Query or validate: " << *route.query << "\n";
    out << "Coverage: " << row->coverage_semantics << "\n"
        << "Absence semantics: " << row->absence_semantics << "\n"
        << "Does not prove:\n";
    for (const auto& claim : row->does_not_prove)
        out << "  - " << claim << "\n";
    return out.str();
}

} // namespace pulp::cli::authority
