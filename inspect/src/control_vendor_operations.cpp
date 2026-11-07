#include <pulp/inspect/control_vendor_operations.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace pulp::inspect {
namespace {

constexpr std::string_view kSchemaDialect = "https://json-schema.org/draft/2020-12/schema";
constexpr std::size_t kMaximumSchemaDepth = 16;
constexpr std::size_t kMaximumSchemaNodes = 2048;
constexpr std::size_t kMaximumStringLength = 65536;
constexpr std::size_t kMaximumArrayItems = 4096;
constexpr std::size_t kMaximumAlternatives = 16;
constexpr std::size_t kMaximumEnumValues = 256;
constexpr std::size_t kMaximumNameSegments = 6;
constexpr std::size_t kMaximumSegmentBytes = 48;
constexpr std::size_t kMaximumLabelBytes = 63;

bool lower_alpha(char c) {
    return c >= 'a' && c <= 'z';
}

bool lower_alnum_or_dash(char c) {
    return lower_alpha(c) || (c >= '0' && c <= '9') || c == '-';
}

// One label/segment: [a-z][a-z0-9-]* that does not end in '-'.
bool valid_segment(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum || !lower_alpha(value.front()) ||
        value.back() == '-')
        return false;
    return std::ranges::all_of(value, lower_alnum_or_dash);
}

bool printable_document(std::string_view document) {
    return std::ranges::all_of(document, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return (byte >= 0x20 && byte <= 0x7e) || c == '\n' || c == '\r' || c == '\t';
    });
}

bool is_integer(const choc::value::ValueView& value) {
    if (value.isInt32() || value.isInt64())
        return true;
    if (value.isFloat64()) {
        const auto number = value.getFloat64();
        return std::isfinite(number) && std::floor(number) == number;
    }
    return false;
}

bool is_number(const choc::value::ValueView& value) {
    return value.isInt32() || value.isInt64() ||
           (value.isFloat64() && std::isfinite(value.getFloat64()));
}

bool is_scalar(const choc::value::ValueView& value) {
    return value.isString() || value.isBool() || is_number(value) || value.isVoid();
}

std::int64_t integer_value(const choc::value::ValueView& value) {
    return value.isFloat64() ? static_cast<std::int64_t>(value.getFloat64()) : value.getInt64();
}

struct SchemaChecker {
    std::size_t nodes = 0;
    std::string error;

    bool fail(std::string path, std::string message) {
        error = (path.empty() ? std::string("schema") : path) + ": " + std::move(message);
        return false;
    }

    bool bounded_count(const choc::value::ValueView& schema, std::string_view keyword,
                       std::size_t maximum, const std::string& path, bool required) {
        if (!schema.hasObjectMember(keyword))
            return !required ||
                   fail(path, std::string(keyword) + " is required to bound this schema");
        const auto value = schema[keyword];
        if (!is_integer(value) || integer_value(value) < 0 ||
            static_cast<std::uint64_t>(integer_value(value)) > maximum)
            return fail(path, std::string(keyword) + " must be an integer from 0 to " +
                                  std::to_string(maximum));
        return true;
    }

    bool check(const choc::value::ValueView& schema, const std::string& path, std::size_t depth,
               bool root) {
        if (++nodes > kMaximumSchemaNodes)
            return fail(path, "schema exceeds the node limit");
        if (depth > kMaximumSchemaDepth)
            return fail(path, "schema exceeds the depth limit");
        if (!schema.isObject())
            return fail(path, "every schema must be a JSON object");

        static constexpr std::array<std::string_view, 17> kKeywords{
            "$schema", "type",     "properties", "required",    "additionalProperties",
            "items",   "minItems", "maxItems",   "uniqueItems", "enum",
            "const",   "minimum",  "maximum",    "minLength",   "maxLength",
            "oneOf",   "default"};
        std::string unknown;
        schema.visitObjectMembers([&](std::string_view name, const choc::value::ValueView&) {
            if (unknown.empty() && std::ranges::find(kKeywords, name) == kKeywords.end())
                unknown = std::string(name);
        });
        if (!unknown.empty())
            return fail(path, "unsupported schema keyword '" + unknown + "'");

        if (schema.hasObjectMember("$schema") && (!root || !schema["$schema"].isString() ||
                                                  schema["$schema"].getString() != kSchemaDialect))
            return fail(path, "$schema may appear only at the root and must name draft 2020-12");

        std::string type;
        if (schema.hasObjectMember("type")) {
            if (!schema["type"].isString())
                return fail(path, "type must be a single string");
            type = std::string(schema["type"].getString());
            static constexpr std::array<std::string_view, 7> kTypes{
                "object", "array", "string", "integer", "number", "boolean", "null"};
            if (std::ranges::find(kTypes, type) == kTypes.end())
                return fail(path, "type '" + type + "' is not supported");
        }
        if (root && type != "object")
            return fail(path, "the top-level schema must have type object");

        const bool has_alternatives = schema.hasObjectMember("oneOf");
        const bool has_enum = schema.hasObjectMember("enum");
        const bool has_const = schema.hasObjectMember("const");
        if (type.empty() && !has_alternatives && !has_enum && !has_const)
            return fail(path, "a schema must declare type, oneOf, enum, or const");

        if (has_enum) {
            const auto values = schema["enum"];
            if (!values.isArray() || values.size() == 0 || values.size() > kMaximumEnumValues)
                return fail(path, "enum must be a non-empty array of at most 256 values");
            for (std::uint32_t index = 0; index < values.size(); ++index)
                if (!is_scalar(values[index]) ||
                    (values[index].isString() &&
                     values[index].getString().size() > kMaximumStringLength))
                    return fail(path, "enum values must be bounded scalars");
        }
        if (has_const && (!is_scalar(schema["const"]) ||
                          (schema["const"].isString() &&
                           schema["const"].getString().size() > kMaximumStringLength)))
            return fail(path, "const must be a bounded scalar");

        if (has_alternatives) {
            const auto alternatives = schema["oneOf"];
            if (!alternatives.isArray() || alternatives.size() == 0 ||
                alternatives.size() > kMaximumAlternatives)
                return fail(path, "oneOf must be a non-empty array of at most 16 schemas");
            for (std::uint32_t index = 0; index < alternatives.size(); ++index)
                if (!check(alternatives[index], path + "/oneOf/" + std::to_string(index), depth + 1,
                           false))
                    return false;
        }

        const bool object_keywords = schema.hasObjectMember("properties") ||
                                     schema.hasObjectMember("required") ||
                                     schema.hasObjectMember("additionalProperties");
        if (object_keywords && type != "object")
            return fail(path, "object keywords require type object");
        if (type == "object") {
            if (!schema.hasObjectMember("additionalProperties") ||
                !schema["additionalProperties"].isBool() ||
                schema["additionalProperties"].getBool())
                return fail(path, "objects must set additionalProperties to false");
            std::set<std::string> declared;
            if (schema.hasObjectMember("properties")) {
                const auto properties = schema["properties"];
                if (!properties.isObject())
                    return fail(path, "properties must be an object");
                bool ok = true;
                properties.visitObjectMembers(
                    [&](std::string_view name, const choc::value::ValueView& property) {
                        if (!ok)
                            return;
                        if (name.empty() || name.size() > 128) {
                            ok = fail(path, "property names must be 1 to 128 bytes");
                            return;
                        }
                        declared.insert(std::string(name));
                        ok = check(property, path + "/properties/" + std::string(name), depth + 1,
                                   false);
                    });
                if (!ok)
                    return false;
            }
            if (schema.hasObjectMember("required")) {
                const auto required = schema["required"];
                if (!required.isArray())
                    return fail(path, "required must be an array");
                for (std::uint32_t index = 0; index < required.size(); ++index)
                    if (!required[index].isString() ||
                        !declared.contains(std::string(required[index].getString())))
                        return fail(path, "required names must be declared properties");
            }
        }

        const bool array_keywords =
            schema.hasObjectMember("items") || schema.hasObjectMember("minItems") ||
            schema.hasObjectMember("maxItems") || schema.hasObjectMember("uniqueItems");
        if (array_keywords && type != "array")
            return fail(path, "array keywords require type array");
        if (type == "array") {
            if (!schema.hasObjectMember("items"))
                return fail(path, "arrays must declare items");
            if (!bounded_count(schema, "maxItems", kMaximumArrayItems, path, true) ||
                !bounded_count(schema, "minItems", kMaximumArrayItems, path, false))
                return false;
            if (schema.hasObjectMember("uniqueItems") && !schema["uniqueItems"].isBool())
                return fail(path, "uniqueItems must be a boolean");
            if (!check(schema["items"], path + "/items", depth + 1, false))
                return false;
        }

        const bool string_keywords =
            schema.hasObjectMember("minLength") || schema.hasObjectMember("maxLength");
        if (string_keywords && type != "string")
            return fail(path, "string keywords require type string");
        if (type == "string" && !has_enum && !has_const) {
            if (!bounded_count(schema, "maxLength", kMaximumStringLength, path, true) ||
                !bounded_count(schema, "minLength", kMaximumStringLength, path, false))
                return false;
        }

        const bool numeric_keywords =
            schema.hasObjectMember("minimum") || schema.hasObjectMember("maximum");
        if (numeric_keywords && type != "integer" && type != "number")
            return fail(path, "numeric keywords require type integer or number");
        for (const auto keyword : {"minimum", "maximum"})
            if (schema.hasObjectMember(keyword) && !is_number(schema[keyword]))
                return fail(path, std::string(keyword) + " must be a finite number");
        return true;
    }
};

} // namespace

std::string_view control_vendor_tier_id(ControlVendorTier tier) {
    switch (tier) {
    case ControlVendorTier::Read:
        return "read";
    case ControlVendorTier::Gesture:
        return "gesture";
    case ControlVendorTier::Render:
        return "render";
    case ControlVendorTier::Destructive:
        return "destructive";
    }
    return {};
}

std::optional<ControlVendorTier> control_vendor_tier_from_id(std::string_view id) {
    for (const auto tier : {ControlVendorTier::Read, ControlVendorTier::Gesture,
                            ControlVendorTier::Render, ControlVendorTier::Destructive})
        if (control_vendor_tier_id(tier) == id)
            return tier;
    return std::nullopt;
}

InspectorCapability control_vendor_tier_capability(ControlVendorTier tier) {
    switch (tier) {
    case ControlVendorTier::Read:
        return InspectorCapability::VendorRead;
    case ControlVendorTier::Gesture:
        return InspectorCapability::VendorGesture;
    case ControlVendorTier::Render:
        return InspectorCapability::VendorRender;
    case ControlVendorTier::Destructive:
        return InspectorCapability::VendorDestructive;
    }
    return InspectorCapability::Unavailable;
}

std::optional<ControlVendorTier>
control_vendor_tier_for_capability(InspectorCapability capability) {
    for (const auto tier : {ControlVendorTier::Read, ControlVendorTier::Gesture,
                            ControlVendorTier::Render, ControlVendorTier::Destructive})
        if (control_vendor_tier_capability(tier) == capability)
            return tier;
    return std::nullopt;
}

bool capability_is_vendor_tier(InspectorCapability capability) {
    return control_vendor_tier_for_capability(capability).has_value();
}

bool control_vendor_id_is_valid(std::string_view vendor_id) {
    if (vendor_id.empty() || vendor_id.size() > kControlVendorIdMaximumBytes)
        return false;
    std::size_t labels = 0;
    std::string_view first;
    std::string_view rest = vendor_id;
    while (true) {
        const auto dot = rest.find('.');
        const auto label = rest.substr(0, dot);
        if (!valid_segment(label, kMaximumLabelBytes))
            return false;
        if (labels == 0)
            first = label;
        ++labels;
        if (dot == std::string_view::npos)
            break;
        rest.remove_prefix(dot + 1);
    }
    if (labels < 2 || labels > 8 || first == "pulp")
        return false;
    return vendor_id != "dev.pulp" && !vendor_id.starts_with("dev.pulp.");
}

std::optional<ControlVendorOperationIdParts>
parse_control_vendor_operation_id(std::string_view operation_id) {
    constexpr std::string_view prefix = "dev.";
    if (operation_id.size() > kControlVendorOperationIdMaximumBytes ||
        !operation_id.starts_with(prefix) || operation_id.starts_with("dev.pulp."))
        return std::nullopt;
    const auto slash = operation_id.find('/');
    const auto at = operation_id.rfind('@');
    if (slash == std::string_view::npos || at == std::string_view::npos || at < slash)
        return std::nullopt;
    ControlVendorOperationIdParts parts;
    parts.vendor_id = operation_id.substr(prefix.size(), slash - prefix.size());
    parts.name = operation_id.substr(slash + 1, at - slash - 1);
    const auto version = operation_id.substr(at + 1);
    if (!control_vendor_id_is_valid(parts.vendor_id) || version.empty() || version.size() > 4 ||
        version.front() == '0' ||
        !std::ranges::all_of(version, [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    for (const char c : version)
        parts.version = parts.version * 10 + static_cast<std::uint32_t>(c - '0');

    std::size_t segments = 0;
    std::string_view rest = parts.name;
    while (true) {
        const auto separator = rest.find_first_of("/.");
        if (!valid_segment(rest.substr(0, separator), kMaximumSegmentBytes) ||
            ++segments > kMaximumNameSegments)
            return std::nullopt;
        if (separator == std::string_view::npos)
            break;
        rest.remove_prefix(separator + 1);
    }
    return parts;
}

bool is_control_vendor_operation_id(std::string_view operation_id) {
    return parse_control_vendor_operation_id(operation_id).has_value();
}

std::string_view control_vendor_operations_error_id(ControlVendorOperationsError error) {
    switch (error) {
    case ControlVendorOperationsError::None:
        return "none";
    case ControlVendorOperationsError::Encoding:
        return "vendor-operations.encoding";
    case ControlVendorOperationsError::Limit:
        return "vendor-operations.limit";
    case ControlVendorOperationsError::Parse:
        return "vendor-operations.parse";
    case ControlVendorOperationsError::UnknownField:
        return "vendor-operations.unknown-field";
    case ControlVendorOperationsError::UnsupportedSchema:
        return "vendor-operations.unsupported-schema";
    case ControlVendorOperationsError::InvalidVendor:
        return "vendor-operations.invalid-vendor";
    case ControlVendorOperationsError::VendorBundleMismatch:
        return "vendor-operations.vendor-bundle-mismatch";
    case ControlVendorOperationsError::InvalidOperation:
        return "vendor-operations.invalid-operation";
    case ControlVendorOperationsError::DuplicateOperation:
        return "vendor-operations.duplicate-operation";
    case ControlVendorOperationsError::InvalidTier:
        return "vendor-operations.invalid-tier";
    case ControlVendorOperationsError::InvalidDescription:
        return "vendor-operations.invalid-description";
    case ControlVendorOperationsError::InvalidSchema:
        return "vendor-operations.invalid-schema";
    }
    return "vendor-operations.parse";
}

ControlVendorOperationTable::ControlVendorOperationTable(
    std::string vendor_id, std::vector<ControlVendorOperation> operations)
    : vendor_id_(std::move(vendor_id)), operations_(std::move(operations)) {
    // operations_ is never resized after this point, so the views below stay
    // valid for the table's lifetime.
    schema_ids_.reserve(operations_.size());
    for (const auto& operation : operations_)
        schema_ids_.push_back({operation.id + "#input", operation.id + "#output"});
    descriptors_.reserve(operations_.size());
    for (std::size_t index = 0; index < operations_.size(); ++index) {
        const auto& operation = operations_[index];
        descriptors_.push_back(ControlOperationDescriptor{
            .id = operation.id,
            .version = operation.version,
            .capability = control_vendor_tier_capability(operation.tier),
            .input_schema_id = schema_ids_[index].first,
            .input_schema_json = operation.input_schema_json,
            .output_schema_id = schema_ids_[index].second,
            .output_schema_json = operation.output_schema_json,
            .result_kind = "response",
            .artifact_binding = {},
            .receipt_binding = {},
        });
    }
}

const ControlVendorOperation* ControlVendorOperationTable::find(std::string_view id,
                                                                std::uint32_t version) const {
    const auto found = std::ranges::find_if(operations_, [&](const auto& operation) {
        return operation.id == id && operation.version == version;
    });
    return found == operations_.end() ? nullptr : &*found;
}

const ControlOperationDescriptor*
ControlVendorOperationTable::descriptor(std::string_view id, std::uint32_t version) const {
    const auto found = std::ranges::find_if(descriptors_, [&](const auto& descriptor) {
        return descriptor.id == id && descriptor.version == version;
    });
    return found == descriptors_.end() ? nullptr : &*found;
}

bool ControlVendorOperationTable::declares_tier(ControlVendorTier tier) const {
    return std::ranges::any_of(operations_,
                               [tier](const auto& operation) { return operation.tier == tier; });
}

bool validate_control_vendor_schema(std::string_view schema_json, std::string& error, bool root) {
    try {
        const auto schema = choc::json::parse(schema_json);
        SchemaChecker checker;
        if (!checker.check(schema, {}, 0, root)) {
            error = checker.error;
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("schema is not valid JSON: ") + exception.what();
        return false;
    }
}

std::shared_ptr<const ControlVendorOperationTable>
parse_control_vendor_operations(std::string_view document, std::string_view bundle_id,
                                ControlVendorOperationsDiagnostics* diagnostics) {
    ControlVendorOperationsDiagnostics local;
    auto& result = diagnostics ? *diagnostics : local;
    result = {};
    const auto fail = [&](ControlVendorOperationsError code, std::string error) {
        result.code = code;
        result.error = std::move(error);
        return std::shared_ptr<const ControlVendorOperationTable>{};
    };
    if (document.empty() || document.size() > kControlVendorOperationsMaximumBytes)
        return fail(ControlVendorOperationsError::Limit,
                    "vendor operation declaration must be 1 to 131072 bytes");
    if (!printable_document(document))
        return fail(ControlVendorOperationsError::Encoding,
                    "vendor operation declaration must be printable ASCII");

    choc::value::Value root;
    try {
        root = choc::json::parse(document);
    } catch (const std::exception& exception) {
        return fail(ControlVendorOperationsError::Parse,
                    std::string("vendor operation declaration is not valid JSON: ") +
                        exception.what());
    }
    if (!root.isObject())
        return fail(ControlVendorOperationsError::Parse,
                    "vendor operation declaration must be a JSON object");
    std::string unknown;
    root.getView().visitObjectMembers([&](std::string_view name, const choc::value::ValueView&) {
        if (unknown.empty() && name != "schema" && name != "vendor" && name != "operations")
            unknown = std::string(name);
    });
    if (!unknown.empty())
        return fail(ControlVendorOperationsError::UnknownField,
                    "vendor operation declaration contains unknown field '" + unknown + "'");
    if (!root["schema"].isString() ||
        root["schema"].getString() != kControlVendorOperationsSchemaId)
        return fail(ControlVendorOperationsError::UnsupportedSchema,
                    "schema must equal " + std::string(kControlVendorOperationsSchemaId));
    if (!root["vendor"].isString() || !control_vendor_id_is_valid(root["vendor"].getString()))
        return fail(ControlVendorOperationsError::InvalidVendor,
                    "vendor must be a reverse-DNS id outside the reserved pulp namespace");
    const auto vendor_id = std::string(root["vendor"].getString());
    if (bundle_id != vendor_id &&
        !(bundle_id.size() > vendor_id.size() && bundle_id.starts_with(vendor_id) &&
          bundle_id[vendor_id.size()] == '.'))
        return fail(ControlVendorOperationsError::VendorBundleMismatch,
                    "vendor must equal the bundle id or one of its reverse-DNS prefixes");

    const auto operations = root["operations"];
    if (!operations.isArray() || operations.size() == 0 ||
        operations.size() > kControlVendorOperationsMaximumCount)
        return fail(ControlVendorOperationsError::Limit,
                    "operations must be an array of 1 to 64 declarations");

    std::vector<ControlVendorOperation> parsed;
    parsed.reserve(operations.size());
    std::set<std::string> ids;
    for (std::uint32_t index = 0; index < operations.size(); ++index) {
        const auto entry = operations[index];
        const auto where = "operations[" + std::to_string(index) + "]";
        if (!entry.isObject())
            return fail(ControlVendorOperationsError::InvalidOperation,
                        where + " must be an object");
        std::string entry_unknown;
        entry.visitObjectMembers([&](std::string_view name, const choc::value::ValueView&) {
            static constexpr std::array<std::string_view, 5> kFields{
                "id", "tier", "description", "input_schema", "output_schema"};
            if (entry_unknown.empty() && std::ranges::find(kFields, name) == kFields.end())
                entry_unknown = std::string(name);
        });
        if (!entry_unknown.empty())
            return fail(ControlVendorOperationsError::UnknownField,
                        where + " contains unknown field '" + entry_unknown + "'");
        if (!entry["id"].isString())
            return fail(ControlVendorOperationsError::InvalidOperation, where + ".id is required");
        ControlVendorOperation operation;
        operation.id = std::string(entry["id"].getString());
        const auto parts = parse_control_vendor_operation_id(operation.id);
        if (!parts || parts->vendor_id != vendor_id)
            return fail(ControlVendorOperationsError::InvalidOperation,
                        where +
                            ".id must be dev.<vendor>/<name>@<version> for the declared vendor");
        operation.version = parts->version;
        if (!ids.insert(operation.id).second)
            return fail(ControlVendorOperationsError::DuplicateOperation,
                        where + ".id is declared more than once");
        const auto tier = entry["tier"].isString()
                              ? control_vendor_tier_from_id(entry["tier"].getString())
                              : std::nullopt;
        if (!tier)
            return fail(ControlVendorOperationsError::InvalidTier,
                        where + ".tier must be read, gesture, render, or destructive");
        operation.tier = *tier;
        if (!entry["description"].isString() || entry["description"].getString().empty() ||
            entry["description"].getString().size() > kControlVendorDescriptionMaximumBytes)
            return fail(ControlVendorOperationsError::InvalidDescription,
                        where + ".description must be 1 to 512 bytes");
        operation.description = std::string(entry["description"].getString());
        for (const auto field : {"input_schema", "output_schema"}) {
            if (!entry.hasObjectMember(field))
                return fail(ControlVendorOperationsError::InvalidSchema,
                            where + "." + field + " is required");
            const auto compact = choc::json::toString(entry[field], false);
            std::string schema_error;
            if (!validate_control_vendor_schema(compact, schema_error))
                return fail(ControlVendorOperationsError::InvalidSchema,
                            where + "." + field + ": " + schema_error);
            (std::string_view(field) == "input_schema" ? operation.input_schema_json
                                                       : operation.output_schema_json) = compact;
        }
        parsed.push_back(std::move(operation));
    }
    return std::make_shared<const ControlVendorOperationTable>(vendor_id, std::move(parsed));
}

} // namespace pulp::inspect
