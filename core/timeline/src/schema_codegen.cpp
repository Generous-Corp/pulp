#include <pulp/timeline/schema_codegen.hpp>

#include <string>
#include <string_view>

namespace pulp::timeline {

namespace {

template <typename T>
runtime::Result<T, PersistenceError> codegen_fail(PersistenceErrorCode code) {
    return runtime::Result<T, PersistenceError>(runtime::Err(PersistenceError{code}));
}

std::string_view domain_name(SchemaDomain domain) noexcept {
    switch (domain) {
    case SchemaDomain::Document:
        return "Document";
    case SchemaDomain::Content:
        return "Content";
    case SchemaDomain::AssetRepresentation:
        return "AssetRepresentation";
    case SchemaDomain::Command:
        return "Command";
    case SchemaDomain::Diagnostic:
        return "Diagnostic";
    }
    return "Document";
}

// JSON-Schema `type` keyword for each registry value kind. Integer-in-string
// kinds project to "string" (their wire form) while the lossless x-pulp-kind
// keeps the exact registry kind for downstream generators.
std::string_view json_type(SchemaValueKind kind) noexcept {
    switch (kind) {
    case SchemaValueKind::Boolean:
        return "boolean";
    case SchemaValueKind::U32:
        return "integer";
    case SchemaValueKind::I64String:
    case SchemaValueKind::U64String:
    case SchemaValueKind::String:
        return "string";
    case SchemaValueKind::Object:
        return "object";
    case SchemaValueKind::Array:
        return "array";
    }
    return "string";
}

std::string_view kind_name(SchemaValueKind kind) noexcept {
    switch (kind) {
    case SchemaValueKind::Boolean:
        return "Boolean";
    case SchemaValueKind::U32:
        return "U32";
    case SchemaValueKind::I64String:
        return "I64String";
    case SchemaValueKind::U64String:
        return "U64String";
    case SchemaValueKind::String:
        return "String";
    case SchemaValueKind::Object:
        return "Object";
    case SchemaValueKind::Array:
        return "Array";
    }
    return "String";
}

void append_migrations(std::string& out, const std::vector<MigrationStep>& steps) {
    out.push_back('[');
    bool first = true;
    for (const auto& step : steps) {
        if (!first)
            out.push_back(',');
        first = false;
        out += "{\"from\":";
        out += std::to_string(step.from_version);
        out += ",\"to\":";
        out += std::to_string(step.to_version);
        out.push_back('}');
    }
    out.push_back(']');
}

} // namespace

runtime::Result<std::string, PersistenceError>
emit_schema_manifest(const SchemaRegistry& registry, std::size_t maximum_bytes) {
    std::string out;
    out.reserve(4096);
    out += "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",";
    out += "\"title\":\"Pulp Timeline Schema Registry\",";
    out += "\"x-pulp-generator\":\"pulp-timeline-schema-emit\",";
    out += "\"x-pulp-manifest-version\":";
    out += std::to_string(schema_manifest_version);
    out += ",\"$defs\":{";

    bool first_type = true;
    for (const auto& type : registry.types()) {
        if (!first_type)
            out.push_back(',');
        first_type = false;

        out += quote_json_string(type.type_name);
        out += ":{\"type\":\"object\",\"title\":";
        out += quote_json_string(type.type_name);
        out += ",\"x-pulp-domain\":";
        out += quote_json_string(domain_name(type.domain));
        out += ",\"x-pulp-current-version\":";
        out += std::to_string(type.current_version);

        out += ",\"properties\":{";
        bool first_field = true;
        for (const auto& field : type.fields) {
            if (!first_field)
                out.push_back(',');
            first_field = false;
            out += quote_json_string(field.name);
            out += ":{\"type\":";
            out += quote_json_string(json_type(field.kind));
            out += ",\"x-pulp-kind\":";
            out += quote_json_string(kind_name(field.kind));
            if (!field.referenced_type.empty()) {
                out += ",\"$ref\":";
                out += quote_json_string("#/$defs/" + field.referenced_type);
            }
            out.push_back('}');
        }
        out.push_back('}');

        // Required set preserves the registry's name-sorted field order.
        bool any_required = false;
        for (const auto& field : type.fields)
            any_required = any_required || field.required;
        if (any_required) {
            out += ",\"required\":[";
            bool first_required = true;
            for (const auto& field : type.fields) {
                if (!field.required)
                    continue;
                if (!first_required)
                    out.push_back(',');
                first_required = false;
                out += quote_json_string(field.name);
            }
            out.push_back(']');
        }

        out += ",\"x-pulp-migrations\":{\"upgrades\":";
        append_migrations(out, type.upgrades);
        out += ",\"downgrades\":";
        append_migrations(out, type.downgrades);
        out.push_back('}');

        out.push_back('}');
    }
    out += "}}";

    // Round-trip through the parser + canonicalizer so the committed artifact
    // is byte-stable independent of the emit order above, and so a malformed
    // emission is caught here rather than shipped. The parser input limit is
    // sized to our own emission so parsing always succeeds; `maximum_bytes` is
    // enforced on the canonical output below, which is the caller-facing bound.
    DecodeLimits limits;
    limits.max_input_bytes = out.size();
    auto parsed = parse_json(out, limits);
    if (!parsed)
        return runtime::Result<std::string, PersistenceError>(runtime::Err(parsed.error()));
    auto canonical = canonicalize_json(parsed.value()->root());
    if (!canonical)
        return runtime::Result<std::string, PersistenceError>(runtime::Err(canonical.error()));
    if (canonical.value().size() > maximum_bytes)
        return codegen_fail<std::string>(PersistenceErrorCode::OutputLimitExceeded);
    return runtime::Result<std::string, PersistenceError>(runtime::Ok(std::move(canonical).value()));
}

} // namespace pulp::timeline
