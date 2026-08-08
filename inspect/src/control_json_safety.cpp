#include "control_protocol_internal.hpp"

#include <algorithm>
#include <vector>

namespace pulp::inspect::control_protocol_detail {

/// Validates complete Unicode scalar encoding. This rejects overlong forms,
/// UTF-16 surrogates, truncated sequences, and values above U+10FFFF.
bool valid_utf8(std::string_view bytes) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t index = 0;
    const auto continuation = [&](std::size_t at) {
        return at < bytes.size() && (data[at] & 0xc0U) == 0x80U;
    };
    while (index < bytes.size()) {
        const auto lead = data[index];
        if (lead <= 0x7fU) {
            ++index;
        } else if (lead >= 0xc2U && lead <= 0xdfU) {
            if (!continuation(index + 1))
                return false;
            index += 2;
        } else if (lead == 0xe0U) {
            if (index + 2 >= bytes.size() || data[index + 1] < 0xa0U || data[index + 1] > 0xbfU ||
                !continuation(index + 2))
                return false;
            index += 3;
        } else if ((lead >= 0xe1U && lead <= 0xecU) || (lead >= 0xeeU && lead <= 0xefU)) {
            if (!continuation(index + 1) || !continuation(index + 2))
                return false;
            index += 3;
        } else if (lead == 0xedU) {
            if (index + 2 >= bytes.size() || data[index + 1] < 0x80U || data[index + 1] > 0x9fU ||
                !continuation(index + 2))
                return false;
            index += 3;
        } else if (lead == 0xf0U) {
            if (index + 3 >= bytes.size() || data[index + 1] < 0x90U || data[index + 1] > 0xbfU ||
                !continuation(index + 2) || !continuation(index + 3))
                return false;
            index += 4;
        } else if (lead >= 0xf1U && lead <= 0xf3U) {
            if (!continuation(index + 1) || !continuation(index + 2) || !continuation(index + 3))
                return false;
            index += 4;
        } else if (lead == 0xf4U) {
            if (index + 3 >= bytes.size() || data[index + 1] < 0x80U || data[index + 1] > 0x8fU ||
                !continuation(index + 2) || !continuation(index + 3))
                return false;
            index += 4;
        } else {
            return false;
        }
    }
    return true;
}

/// Performs the bounded string-token checks that CHOC's parser assumes its
/// caller has already established. In particular, its string reader can walk
/// past a truncated closing quote before it reports a parse error. This scan
/// never dereferences beyond the supplied view and rejects malformed/truncated
/// escape sequences and raw control characters inside strings.
bool valid_json_string_lexemes(std::string_view bytes) {
    const auto hex = [](unsigned char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
               (value >= 'A' && value <= 'F');
    };
    bool in_string = false;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = static_cast<unsigned char>(bytes[index]);
        if (!in_string) {
            if (value == '"')
                in_string = true;
            continue;
        }
        if (value == '"') {
            in_string = false;
            continue;
        }
        if (value < 0x20U)
            return false;
        if (value != '\\')
            continue;
        if (++index >= bytes.size())
            return false;
        const auto escape = static_cast<unsigned char>(bytes[index]);
        if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' ||
            escape == 'n' || escape == 'r' || escape == 't') {
            continue;
        }
        if (escape != 'u' || index + 4 >= bytes.size())
            return false;
        for (std::size_t digit = 1; digit <= 4; ++digit)
            if (!hex(static_cast<unsigned char>(bytes[index + digit])))
                return false;
        index += 4;
    }
    return !in_string;
}

/// CHOC builds Value containers while it parses and assumes the JSON grammar
/// has a closing token. A small non-allocating recognizer keeps truncated and
/// structurally malformed input away from those internals. It also enforces the
/// same depth/node ceilings used after parsing, so the preflight itself is
/// bounded for adversarial input.
class JsonSyntaxPreflight {
  public:
    JsonSyntaxPreflight(std::string_view bytes, std::size_t maximum_nodes)
        : bytes_(bytes), maximum_nodes_(maximum_nodes) {}

    bool valid() {
        skip_whitespace();
        if (!value(0))
            return false;
        skip_whitespace();
        return at_ == bytes_.size();
    }

  private:
    bool consume(char expected) {
        if (at_ >= bytes_.size() || bytes_[at_] != expected)
            return false;
        ++at_;
        return true;
    }

    void skip_whitespace() {
        while (at_ < bytes_.size() && (bytes_[at_] == ' ' || bytes_[at_] == '\t' ||
                                       bytes_[at_] == '\n' || bytes_[at_] == '\r'))
            ++at_;
    }

    bool value(std::size_t depth) {
        if (depth > kMaximumJsonDepth || nodes_ >= maximum_nodes_)
            return false;
        ++nodes_;
        skip_whitespace();
        if (at_ >= bytes_.size())
            return false;
        switch (bytes_[at_]) {
        case '{':
            return object(depth + 1);
        case '[':
            return array(depth + 1);
        case '"':
            return string();
        case 't':
            return literal("true");
        case 'f':
            return literal("false");
        case 'n':
            return literal("null");
        default:
            return number();
        }
    }

    bool object(std::size_t depth) {
        ++at_;
        skip_whitespace();
        if (consume('}'))
            return true;
        while (true) {
            if (!string())
                return false;
            skip_whitespace();
            if (!consume(':'))
                return false;
            if (!value(depth))
                return false;
            skip_whitespace();
            if (consume('}'))
                return true;
            if (!consume(','))
                return false;
            skip_whitespace();
        }
    }

    bool array(std::size_t depth) {
        ++at_;
        skip_whitespace();
        if (consume(']'))
            return true;
        while (true) {
            if (!value(depth))
                return false;
            skip_whitespace();
            if (consume(']'))
                return true;
            if (!consume(','))
                return false;
            skip_whitespace();
        }
    }

    static int hex_value(unsigned char value) {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    }

    bool unicode_escape(std::uint32_t& code_unit) {
        if (at_ + 4 > bytes_.size())
            return false;
        code_unit = 0;
        for (std::size_t digit = 0; digit < 4; ++digit) {
            const auto nibble = hex_value(static_cast<unsigned char>(bytes_[at_ + digit]));
            if (nibble < 0)
                return false;
            code_unit = (code_unit << 4U) | static_cast<std::uint32_t>(nibble);
        }
        at_ += 4;
        return true;
    }

    bool string() {
        if (!consume('"'))
            return false;
        while (at_ < bytes_.size()) {
            const auto value = static_cast<unsigned char>(bytes_[at_++]);
            if (value == '"')
                return true;
            if (value < 0x20U)
                return false;
            if (value != '\\')
                continue;
            if (at_ >= bytes_.size())
                return false;
            const auto escape = bytes_[at_++];
            if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' ||
                escape == 'f' || escape == 'n' || escape == 'r' || escape == 't')
                continue;
            if (escape != 'u')
                return false;
            std::uint32_t first = 0;
            if (!unicode_escape(first))
                return false;
            if (first >= 0xdc00U && first <= 0xdfffU)
                return false;
            if (first >= 0xd800U && first <= 0xdbffU) {
                if (at_ + 2 > bytes_.size() || bytes_[at_] != '\\' || bytes_[at_ + 1] != 'u')
                    return false;
                at_ += 2;
                std::uint32_t second = 0;
                if (!unicode_escape(second) || second < 0xdc00U || second > 0xdfffU)
                    return false;
            }
        }
        return false;
    }

    bool literal(std::string_view expected) {
        if (bytes_.substr(at_, expected.size()) != expected)
            return false;
        at_ += expected.size();
        return true;
    }

    bool number() {
        const auto begin = at_;
        if (at_ < bytes_.size() && bytes_[at_] == '-')
            ++at_;
        if (at_ >= bytes_.size())
            return false;
        if (bytes_[at_] == '0') {
            ++at_;
            if (at_ < bytes_.size() && bytes_[at_] >= '0' && bytes_[at_] <= '9')
                return false;
        } else if (bytes_[at_] >= '1' && bytes_[at_] <= '9') {
            do {
                ++at_;
            } while (at_ < bytes_.size() && bytes_[at_] >= '0' && bytes_[at_] <= '9');
        } else {
            return false;
        }
        if (at_ < bytes_.size() && bytes_[at_] == '.') {
            ++at_;
            const auto fraction = at_;
            while (at_ < bytes_.size() && bytes_[at_] >= '0' && bytes_[at_] <= '9')
                ++at_;
            if (fraction == at_)
                return false;
        }
        if (at_ < bytes_.size() && (bytes_[at_] == 'e' || bytes_[at_] == 'E')) {
            ++at_;
            if (at_ < bytes_.size() && (bytes_[at_] == '+' || bytes_[at_] == '-'))
                ++at_;
            const auto exponent = at_;
            while (at_ < bytes_.size() && bytes_[at_] >= '0' && bytes_[at_] <= '9')
                ++at_;
            if (exponent == at_)
                return false;
        }
        return at_ > begin;
    }

    std::string_view bytes_;
    std::size_t maximum_nodes_;
    std::size_t at_ = 0;
    std::size_t nodes_ = 0;
};

bool valid_json_syntax(std::string_view bytes, std::size_t maximum_nodes) {
    return JsonSyntaxPreflight(bytes, maximum_nodes).valid();
}

choc::value::Value canonical_value(ValueView value) {
    if (value.isObject()) {
        std::vector<std::pair<std::string, ValueView>> members;
        members.reserve(value.size());
        for (std::uint32_t index = 0; index < value.size(); ++index) {
            const auto member = value.getObjectMemberAt(index);
            members.emplace_back(member.name, member.value);
        }
        std::ranges::sort(members, {}, &std::pair<std::string, ValueView>::first);
        auto result = choc::value::createObject("");
        for (const auto& [name, member] : members)
            result.addMember(name, canonical_value(member));
        return result;
    }
    if (value.isArray()) {
        auto result = choc::value::createEmptyArray();
        for (std::uint32_t index = 0; index < value.size(); ++index)
            result.addArrayElement(canonical_value(value[index]));
        return result;
    }
    if (value.isString())
        return choc::value::createString(value.getString());
    if (value.isInt32())
        return choc::value::createInt32(value.getInt32());
    if (value.isInt64())
        return choc::value::createInt64(value.getInt64());
    if (value.isFloat32())
        return choc::value::createFloat32(value.getFloat32());
    if (value.isFloat64())
        return choc::value::createFloat64(value.getFloat64());
    if (value.isBool())
        return choc::value::createBool(value.getBool());
    return {};
}

bool bounded_json_shape(ValueView value, std::size_t depth, std::size_t& remaining_nodes) {
    if (depth > kMaximumJsonDepth || remaining_nodes == 0)
        return false;
    --remaining_nodes;
    if (value.isObject()) {
        for (std::uint32_t index = 0; index < value.size(); ++index)
            if (!bounded_json_shape(value.getObjectMemberAt(index).value, depth + 1,
                                    remaining_nodes))
                return false;
    } else if (value.isArray()) {
        for (std::uint32_t index = 0; index < value.size(); ++index)
            if (!bounded_json_shape(value[index], depth + 1, remaining_nodes))
                return false;
    }
    return true;
}

bool valid_control_json_bytes(std::string_view bytes, std::size_t maximum_bytes,
                              std::size_t maximum_nodes) {
    return !bytes.empty() && bytes.size() <= maximum_bytes && valid_utf8(bytes) &&
           valid_json_string_lexemes(bytes) && valid_json_syntax(bytes, maximum_nodes);
}

std::optional<choc::value::Value> parse_bounded_control_json(std::string_view json,
                                                             std::size_t maximum_bytes,
                                                             std::size_t maximum_nodes) {
    if (!valid_control_json_bytes(json, maximum_bytes, maximum_nodes))
        return std::nullopt;
    try {
        const std::string terminated(json);
        auto parsed = choc::json::parseValue(terminated);
        std::size_t remaining_nodes = maximum_nodes;
        if (!bounded_json_shape(parsed, 0, remaining_nodes))
            return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace pulp::inspect::control_protocol_detail

namespace pulp::inspect {

std::optional<std::string> canonicalize_control_json(std::string_view json) {
    const auto parsed = control_protocol_detail::parse_bounded_control_json(
        json, control_protocol_detail::kMaximumPayloadBytes);
    if (!parsed)
        return std::nullopt;
    return choc::json::toString(control_protocol_detail::canonical_value(*parsed), false);
}

namespace control_protocol_detail {

std::optional<std::string> canonicalize_control_result_json(std::string_view json) {
    const auto parsed =
        parse_bounded_control_json(json, kControlMaximumResultDetailBytes, kMaximumResultJsonNodes);
    if (!parsed)
        return std::nullopt;
    return choc::json::toString(canonical_value(*parsed), false);
}

} // namespace control_protocol_detail

} // namespace pulp::inspect
