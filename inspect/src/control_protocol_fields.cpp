#include "control_protocol_internal.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace pulp::inspect::control_protocol_detail {

bool only_fields(ValueView value, std::initializer_list<std::string_view> allowed,
                 ControlProtocolDiagnostics& diagnostics) {
    if (!value.isObject()) {
        diagnostics = {ControlProtocolError::InvalidType, "value must be an object"};
        return false;
    }
    std::set<std::string_view> seen;
    for (std::uint32_t index = 0; index < value.size(); ++index) {
        const auto member = value.getObjectMemberAt(index);
        if (!seen.insert(member.name).second) {
            diagnostics = {ControlProtocolError::InvalidValue,
                           "duplicate field '" + std::string(member.name) + "'"};
            return false;
        }
        if (std::ranges::find(allowed, member.name) == allowed.end()) {
            diagnostics = {ControlProtocolError::UnknownField,
                           "unknown field '" + std::string(member.name) + "'"};
            return false;
        }
    }
    return true;
}

bool required_string(ValueView value, std::string_view name, std::string& out, std::size_t maximum,
                     ControlProtocolDiagnostics& diagnostics, bool token) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isString()) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be a string"};
        return false;
    }
    out = std::string(field.getString());
    if ((token && !valid_token(out, maximum)) || (!token && !valid_text(out, maximum))) {
        diagnostics = {ControlProtocolError::InvalidValue,
                       "field '" + std::string(name) + "' is invalid"};
        return false;
    }
    return true;
}

bool required_u32(ValueView value, std::string_view name, std::uint32_t& out,
                  ControlProtocolDiagnostics& diagnostics, bool nonzero) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isInt()) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be an integer"};
        return false;
    }
    const auto number = field.getInt64();
    if (number < (nonzero ? 1 : 0) || number > std::numeric_limits<std::uint32_t>::max()) {
        diagnostics = {ControlProtocolError::InvalidValue,
                       "field '" + std::string(name) + "' is out of range"};
        return false;
    }
    out = static_cast<std::uint32_t>(number);
    return true;
}

bool required_u64(ValueView value, std::string_view name, std::uint64_t& out,
                  ControlProtocolDiagnostics& diagnostics) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isInt() || field.getInt64() < 0) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be a non-negative integer"};
        return false;
    }
    out = static_cast<std::uint64_t>(field.getInt64());
    return true;
}

bool required_i64(ValueView value, std::string_view name, std::int64_t& out,
                  ControlProtocolDiagnostics& diagnostics) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isInt()) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be an integer"};
        return false;
    }
    out = field.getInt64();
    return true;
}

bool required_bool(ValueView value, std::string_view name, bool& out,
                   ControlProtocolDiagnostics& diagnostics) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isBool()) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be a boolean"};
        return false;
    }
    out = field.getBool();
    return true;
}

bool parse_features(ValueView value, std::string_view name, std::vector<std::string>& out,
                    ControlProtocolDiagnostics& diagnostics) {
    if (!value.hasObjectMember(name)) {
        diagnostics = {ControlProtocolError::MissingField,
                       "missing field '" + std::string(name) + "'"};
        return false;
    }
    const auto field = value[name];
    if (!field.isArray()) {
        diagnostics = {ControlProtocolError::InvalidType,
                       "field '" + std::string(name) + "' must be an array"};
        return false;
    }
    if (field.size() > kMaximumFeatures) {
        diagnostics = {ControlProtocolError::LimitExceeded, "feature limit exceeded"};
        return false;
    }
    for (std::uint32_t index = 0; index < field.size(); ++index) {
        if (!field[index].isString()) {
            diagnostics = {ControlProtocolError::InvalidType, "features must be strings"};
            return false;
        }
        out.emplace_back(field[index].getString());
    }
    if (!valid_features(out)) {
        diagnostics = {ControlProtocolError::InvalidValue, "features are invalid or duplicated"};
        return false;
    }
    return true;
}

choc::value::Value string_array(const std::vector<std::string>& strings) {
    auto result = choc::value::createEmptyArray();
    for (const auto& string : strings)
        result.addArrayElement(choc::value::createString(string));
    return result;
}

} // namespace pulp::inspect::control_protocol_detail
