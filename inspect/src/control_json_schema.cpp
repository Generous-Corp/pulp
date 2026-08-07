#include "control_protocol_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <regex>
#include <set>

namespace pulp::inspect {
namespace {

using control_protocol_detail::ValueView;

constexpr std::size_t kMaximumSchemaBytes = 256u * 1024u;
constexpr std::size_t kMaximumFallbackRegexInputBytes = 4096;

void fail(ControlJsonSchemaDiagnostics& diagnostics, ControlJsonSchemaError code, std::string path,
          std::string explanation) {
    if (diagnostics.code != ControlJsonSchemaError::None)
        return;
    diagnostics.code = code;
    diagnostics.path = std::move(path);
    diagnostics.explanation = std::move(explanation);
}

bool matches_control_pattern(std::string_view text, std::string_view pattern,
                             ControlJsonSchemaDiagnostics& diagnostics, std::string_view path) {
    if (pattern == R"(^[^\u0000]*$)")
        return text.find('\0') == std::string_view::npos;

    if (pattern == R"(^[0-9a-f]{64}$)") {
        return text.size() == 64 && std::ranges::all_of(text, [](unsigned char byte) {
                   return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
               });
    }

    if (pattern == R"(^[A-Za-z0-9+/]*={0,2}$)") {
        const auto padding = text.find('=');
        const auto body = text.substr(0, padding);
        const auto suffix =
            padding == std::string_view::npos ? std::string_view{} : text.substr(padding);
        return suffix.size() <= 2 &&
               std::ranges::all_of(body,
                                   [](unsigned char byte) {
                                       return (byte >= 'A' && byte <= 'Z') ||
                                              (byte >= 'a' && byte <= 'z') ||
                                              (byte >= '0' && byte <= '9') || byte == '+' ||
                                              byte == '/';
                                   }) &&
               std::ranges::all_of(suffix, [](unsigned char byte) { return byte == '='; });
    }

    if (pattern == R"(^[A-Za-z_][A-Za-z0-9_.-]*$)") {
        const auto initial = [](unsigned char byte) {
            return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || byte == '_';
        };
        const auto continuation = [&](unsigned char byte) {
            return initial(byte) || (byte >= '0' && byte <= '9') || byte == '.' || byte == '-';
        };
        return !text.empty() && initial(static_cast<unsigned char>(text.front())) &&
               std::ranges::all_of(text.substr(1), continuation);
    }

    // libstdc++'s recursive regex executor can overflow the stack on long strings.
    // Registry patterns above have linear, allocation-free matchers; keep the
    // general schema fallback bounded for callers supplying their own schemas.
    if (text.size() > kMaximumFallbackRegexInputBytes) {
        fail(diagnostics, ControlJsonSchemaError::LimitExceeded, std::string(path),
             "pattern input exceeds the bounded regex fallback");
        return false;
    }
    return std::regex_search(text.begin(), text.end(), std::regex(std::string(pattern)));
}

bool is_number(ValueView value) {
    return value.isInt32() || value.isInt64() || value.isFloat32() || value.isFloat64();
}

double number_value(ValueView value) {
    if (value.isInt32())
        return static_cast<double>(value.getInt32());
    if (value.isInt64())
        return static_cast<double>(value.getInt64());
    if (value.isFloat32())
        return static_cast<double>(value.getFloat32());
    return value.getFloat64();
}

std::optional<std::int64_t> integer_value(ValueView value) {
    if (value.isInt32())
        return static_cast<std::int64_t>(value.getInt32());
    if (value.isInt64())
        return value.getInt64();
    return std::nullopt;
}

int compare_integer_to_floating(std::int64_t integer, double floating) {
    constexpr double minimum = -9223372036854775808.0;
    constexpr double upper_exclusive = 9223372036854775808.0;
    if (floating < minimum)
        return 1;
    if (floating >= upper_exclusive)
        return -1;

    const auto truncated = static_cast<std::int64_t>(floating);
    if (integer < truncated)
        return -1;
    if (integer > truncated)
        return 1;
    if (floating == static_cast<double>(truncated))
        return 0;
    return floating > 0 ? -1 : 1;
}

int compare_numbers(ValueView left, ValueView right) {
    const auto left_integer = integer_value(left);
    const auto right_integer = integer_value(right);
    if (left_integer && right_integer) {
        if (*left_integer < *right_integer)
            return -1;
        return *left_integer > *right_integer ? 1 : 0;
    }
    if (left_integer)
        return compare_integer_to_floating(*left_integer, number_value(right));
    if (right_integer)
        return -compare_integer_to_floating(*right_integer, number_value(left));
    const auto left_number = number_value(left);
    const auto right_number = number_value(right);
    if (left_number < right_number)
        return -1;
    return left_number > right_number ? 1 : 0;
}

bool is_json_integer(ValueView value) {
    if (value.isInt32() || value.isInt64())
        return true;
    if (!value.isFloat32() && !value.isFloat64())
        return false;
    const auto number = number_value(value);
    return std::isfinite(number) && std::trunc(number) == number;
}

std::optional<std::uint64_t> nonnegative_integer(ValueView value) {
    if (value.isInt32()) {
        const auto number = value.getInt32();
        if (number < 0)
            return std::nullopt;
        return static_cast<std::uint64_t>(number);
    }
    if (value.isInt64()) {
        const auto number = value.getInt64();
        if (number < 0)
            return std::nullopt;
        return static_cast<std::uint64_t>(number);
    }
    return std::nullopt;
}

std::size_t utf8_length(std::string_view value) {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xc0U) != 0x80U; }));
}

std::string child_path(std::string_view path, std::string_view child) {
    std::string result(path);
    result.push_back('/');
    for (const auto character : child) {
        if (character == '~')
            result += "~0";
        else if (character == '/')
            result += "~1";
        else
            result.push_back(character);
    }
    return result;
}

bool equal_json(ValueView left, ValueView right) {
    return choc::json::toString(control_protocol_detail::canonical_value(left), false) ==
           choc::json::toString(control_protocol_detail::canonical_value(right), false);
}

bool validate_schema_definition(ValueView schema, std::size_t depth, std::size_t& remaining,
                                ControlJsonSchemaDiagnostics& diagnostics, std::string_view path) {
    if (depth > control_protocol_detail::kMaximumJsonDepth || remaining == 0) {
        fail(diagnostics, ControlJsonSchemaError::LimitExceeded, std::string(path),
             "schema exceeds validation limits");
        return false;
    }
    --remaining;
    if (!schema.isObject()) {
        fail(diagnostics, ControlJsonSchemaError::InvalidSchema, std::string(path),
             "schema node must be an object");
        return false;
    }

    static const std::set<std::string_view> supported{
        "$schema",
        "additionalProperties",
        "allOf",
        "anyOf",
        "const",
        "default",
        "enum",
        "if",
        "items",
        "maxItems",
        "maxLength",
        "maxProperties",
        "maximum",
        "minItems",
        "minLength",
        "minProperties",
        "minimum",
        "not",
        "oneOf",
        "pattern",
        "properties",
        "propertyNames",
        "required",
        "then",
        "type",
        "uniqueItems",
        "x-pulp-maxUtf8Bytes",
    };
    std::set<std::string_view> seen;
    bool valid = true;
    schema.visitObjectMembers([&](std::string_view name, ValueView value) {
        if (!valid)
            return;
        if (!seen.insert(name).second) {
            fail(diagnostics, ControlJsonSchemaError::InvalidSchema, child_path(path, name),
                 "duplicate schema keyword");
            valid = false;
            return;
        }
        if (!supported.contains(name)) {
            fail(diagnostics, ControlJsonSchemaError::UnsupportedKeyword, child_path(path, name),
                 "unsupported schema keyword");
            valid = false;
            return;
        }
        if (name == "$schema") {
            valid = value.isString() &&
                    value.getString() == "https://json-schema.org/draft/2020-12/schema";
        } else if (name == "type") {
            static const std::set<std::string_view> types{"object", "array",   "string",
                                                          "number", "integer", "boolean"};
            valid = value.isString() && types.contains(value.getString());
        } else if (name == "properties") {
            valid = value.isObject();
            if (valid) {
                std::set<std::string_view> property_names;
                value.visitObjectMembers([&](std::string_view property, ValueView child) {
                    if (!valid || !property_names.insert(property).second) {
                        valid = false;
                        return;
                    }
                    valid =
                        validate_schema_definition(child, depth + 1, remaining, diagnostics,
                                                   child_path(child_path(path, name), property));
                });
            }
        } else if (name == "items" || name == "not" || name == "if" || name == "then" ||
                   name == "propertyNames") {
            valid = validate_schema_definition(value, depth + 1, remaining, diagnostics,
                                               child_path(path, name));
        } else if (name == "additionalProperties") {
            valid =
                value.isBool() || validate_schema_definition(value, depth + 1, remaining,
                                                             diagnostics, child_path(path, name));
        } else if (name == "oneOf" || name == "anyOf" || name == "allOf") {
            valid = value.isArray() && value.size() > 0;
            for (std::uint32_t index = 0; valid && index < value.size(); ++index) {
                valid = validate_schema_definition(
                    value[index], depth + 1, remaining, diagnostics,
                    child_path(child_path(path, name), std::to_string(index)));
            }
        } else if (name == "required") {
            valid = value.isArray();
            std::set<std::string_view> required;
            for (std::uint32_t index = 0; valid && index < value.size(); ++index)
                valid = value[index].isString() && required.insert(value[index].getString()).second;
        } else if (name == "enum") {
            valid = value.isArray() && value.size() > 0;
        } else if (name == "pattern") {
            valid = value.isString();
            if (valid) {
                try {
                    (void)std::regex(std::string(value.getString()));
                } catch (...) {
                    valid = false;
                }
            }
        } else if (name == "minimum" || name == "maximum") {
            valid = is_number(value) && std::isfinite(number_value(value));
        } else if (name == "minItems" || name == "maxItems" || name == "minLength" ||
                   name == "maxLength" || name == "minProperties" || name == "maxProperties" ||
                   name == "x-pulp-maxUtf8Bytes") {
            valid = nonnegative_integer(value).has_value();
        } else if (name == "uniqueItems") {
            valid = value.isBool();
        } else {
            // `const` and `default` intentionally accept any bounded JSON value.
            valid = true;
        }
        if (!valid && diagnostics.code == ControlJsonSchemaError::None) {
            fail(diagnostics, ControlJsonSchemaError::InvalidSchema, child_path(path, name),
                 "schema keyword has an invalid value");
        }
    });
    return valid;
}

bool validate_instance(ValueView instance, ValueView schema,
                       ControlJsonSchemaDiagnostics& diagnostics, std::string_view path,
                       std::size_t depth) {
    if (depth > control_protocol_detail::kMaximumJsonDepth) {
        fail(diagnostics, ControlJsonSchemaError::LimitExceeded, std::string(path),
             "instance exceeds validation depth");
        return false;
    }
    const auto matches = [&](ValueView branch) {
        ControlJsonSchemaDiagnostics ignored;
        return validate_instance(instance, branch, ignored, path, depth + 1);
    };
    if (schema.hasObjectMember("allOf")) {
        const auto branches = schema["allOf"];
        for (std::uint32_t index = 0; index < branches.size(); ++index)
            if (!matches(branches[index]))
                goto validation_failed;
    }
    if (schema.hasObjectMember("anyOf")) {
        const auto branches = schema["anyOf"];
        bool matched = false;
        for (std::uint32_t index = 0; index < branches.size(); ++index)
            matched = matched || matches(branches[index]);
        if (!matched)
            goto validation_failed;
    }
    if (schema.hasObjectMember("oneOf")) {
        const auto branches = schema["oneOf"];
        std::size_t matches_count = 0;
        for (std::uint32_t index = 0; index < branches.size(); ++index)
            if (matches(branches[index]))
                ++matches_count;
        if (matches_count != 1)
            goto validation_failed;
    }
    if (schema.hasObjectMember("not") && matches(schema["not"]))
        goto validation_failed;
    if (schema.hasObjectMember("if") && matches(schema["if"]) && schema.hasObjectMember("then") &&
        !matches(schema["then"]))
        goto validation_failed;

    if (schema.hasObjectMember("type")) {
        const auto type = schema["type"].getString();
        const bool correct = (type == "object" && instance.isObject()) ||
                             (type == "array" && instance.isArray()) ||
                             (type == "string" && instance.isString()) ||
                             (type == "number" && is_number(instance)) ||
                             (type == "integer" && is_json_integer(instance)) ||
                             (type == "boolean" && instance.isBool());
        if (!correct)
            goto validation_failed;
    }
    if (schema.hasObjectMember("const") && !equal_json(instance, schema["const"]))
        goto validation_failed;
    if (schema.hasObjectMember("enum")) {
        bool found = false;
        const auto values = schema["enum"];
        for (std::uint32_t index = 0; index < values.size(); ++index)
            found = found || equal_json(instance, values[index]);
        if (!found)
            goto validation_failed;
    }

    if (instance.isObject()) {
        std::set<std::string_view> names;
        bool valid = true;
        const auto properties = schema["properties"];
        const auto additional = schema["additionalProperties"];
        instance.visitObjectMembers([&](std::string_view name, ValueView value) {
            if (!valid || !names.insert(name).second) {
                valid = false;
                return;
            }
            if (schema.hasObjectMember("propertyNames") &&
                !validate_instance(choc::value::createString(name), schema["propertyNames"],
                                   diagnostics, child_path(path, name), depth + 1)) {
                valid = false;
                return;
            }
            if (properties.isObject() && properties.hasObjectMember(name)) {
                valid = validate_instance(value, properties[name], diagnostics,
                                          child_path(path, name), depth + 1);
            } else if (additional.isBool()) {
                valid = additional.getBool();
            } else if (additional.isObject()) {
                valid = validate_instance(value, additional, diagnostics, child_path(path, name),
                                          depth + 1);
            }
        });
        if (!valid)
            goto validation_failed;
        if (schema.hasObjectMember("required")) {
            const auto required = schema["required"];
            for (std::uint32_t index = 0; index < required.size(); ++index)
                if (!instance.hasObjectMember(required[index].getString()))
                    goto validation_failed;
        }
        if (schema.hasObjectMember("minProperties") &&
            instance.size() < *nonnegative_integer(schema["minProperties"]))
            goto validation_failed;
        if (schema.hasObjectMember("maxProperties") &&
            instance.size() > *nonnegative_integer(schema["maxProperties"]))
            goto validation_failed;
    }

    if (instance.isArray()) {
        if (schema.hasObjectMember("minItems") &&
            instance.size() < *nonnegative_integer(schema["minItems"]))
            goto validation_failed;
        if (schema.hasObjectMember("maxItems") &&
            instance.size() > *nonnegative_integer(schema["maxItems"]))
            goto validation_failed;
        if (schema.hasObjectMember("items")) {
            for (std::uint32_t index = 0; index < instance.size(); ++index)
                if (!validate_instance(instance[index], schema["items"], diagnostics,
                                       child_path(path, std::to_string(index)), depth + 1))
                    return false;
        }
        if (schema.hasObjectMember("uniqueItems") && schema["uniqueItems"].getBool()) {
            std::set<std::string> unique;
            for (std::uint32_t index = 0; index < instance.size(); ++index)
                if (!unique
                         .insert(choc::json::toString(
                             control_protocol_detail::canonical_value(instance[index]), false))
                         .second)
                    goto validation_failed;
        }
    }

    if (instance.isString()) {
        const auto text = instance.getString();
        const auto length = utf8_length(text);
        if (schema.hasObjectMember("minLength") &&
            length < *nonnegative_integer(schema["minLength"]))
            goto validation_failed;
        if (schema.hasObjectMember("maxLength") &&
            length > *nonnegative_integer(schema["maxLength"]))
            goto validation_failed;
        if (schema.hasObjectMember("x-pulp-maxUtf8Bytes") &&
            text.size() > *nonnegative_integer(schema["x-pulp-maxUtf8Bytes"]))
            goto validation_failed;
        if (schema.hasObjectMember("pattern")) {
            try {
                if (!matches_control_pattern(text, schema["pattern"].getString(), diagnostics,
                                             path))
                    goto validation_failed;
            } catch (...) {
                fail(diagnostics, ControlJsonSchemaError::InvalidSchema, std::string(path),
                     "schema pattern is invalid");
                return false;
            }
        }
    }

    if (is_number(instance)) {
        const auto number = number_value(instance);
        if (!std::isfinite(number))
            goto validation_failed;
        if (schema.hasObjectMember("minimum") && compare_numbers(instance, schema["minimum"]) < 0)
            goto validation_failed;
        if (schema.hasObjectMember("maximum") && compare_numbers(instance, schema["maximum"]) > 0)
            goto validation_failed;
    }
    return true;

validation_failed:
    fail(diagnostics, ControlJsonSchemaError::ValidationFailed, std::string(path),
         "JSON value does not satisfy the operation schema");
    return false;
}

} // namespace

bool validate_with_limits(std::string_view instance_json, std::string_view schema_json,
                          std::size_t maximum_bytes, std::size_t maximum_nodes,
                          ControlJsonSchemaDiagnostics* diagnostics) {
    ControlJsonSchemaDiagnostics local;
    auto& result = diagnostics ? *diagnostics : local;
    result = {};
    const auto instance = control_protocol_detail::parse_bounded_control_json(
        instance_json, maximum_bytes, maximum_nodes);
    if (!instance) {
        fail(result, ControlJsonSchemaError::InvalidDocument, "",
             "instance is not bounded valid JSON");
        return false;
    }
    const auto schema =
        control_protocol_detail::parse_bounded_control_json(schema_json, kMaximumSchemaBytes);
    if (!schema) {
        fail(result, ControlJsonSchemaError::InvalidSchema, "", "schema is not bounded valid JSON");
        return false;
    }
    std::size_t remaining = control_protocol_detail::kMaximumJsonNodes;
    if (!validate_schema_definition(*schema, 0, remaining, result, ""))
        return false;
    return validate_instance(*instance, *schema, result, "", 0);
}

bool validate_control_json_schema(std::string_view instance_json, std::string_view schema_json,
                                  ControlJsonSchemaDiagnostics* diagnostics) {
    return validate_with_limits(instance_json, schema_json,
                                control_protocol_detail::kMaximumPayloadBytes,
                                control_protocol_detail::kMaximumJsonNodes, diagnostics);
}

bool validate_control_output_json_schema(std::string_view instance_json,
                                         std::string_view schema_json,
                                         ControlJsonSchemaDiagnostics* diagnostics) {
    return validate_with_limits(instance_json, schema_json, kControlMaximumResultDetailBytes,
                                control_protocol_detail::kMaximumResultJsonNodes, diagnostics);
}

} // namespace pulp::inspect
