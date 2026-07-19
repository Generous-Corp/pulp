#include <pulp/timeline/schema_json.hpp>

#include "schema_json_write_internal.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::size_t offset = 0,
                                          std::uint64_t actual = 0,
                                          std::uint64_t limit = 0,
                                          std::string path = {}) {
    return runtime::Result<T, PersistenceError>(runtime::Err(
        PersistenceError{code, offset, actual, limit, std::move(path), std::nullopt}));
}

runtime::Result<std::string, PersistenceError> canonical(const JsonValue& value) {
    switch (value.kind) {
        case JsonValue::Kind::Null:
            return runtime::Result<std::string, PersistenceError>(runtime::Ok(std::string("null")));
        case JsonValue::Kind::Boolean:
            return runtime::Result<std::string, PersistenceError>(
                runtime::Ok(std::string(value.boolean ? "true" : "false")));
        case JsonValue::Kind::String:
            return runtime::Result<std::string, PersistenceError>(
                runtime::Ok(quote_json_string(value.scalar)));
        case JsonValue::Kind::Number: {
            if (value.scalar.find_first_of(".eE") != std::string::npos)
                return fail<std::string>(PersistenceErrorCode::InvalidNumber, value.begin);
            std::int64_t number = 0;
            const auto parsed = std::from_chars(value.scalar.data(),
                                                value.scalar.data() + value.scalar.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.scalar.data() + value.scalar.size())
                return fail<std::string>(PersistenceErrorCode::InvalidNumber, value.begin);
            return runtime::Result<std::string, PersistenceError>(
                runtime::Ok(std::to_string(number)));
        }
        case JsonValue::Kind::Array: {
            std::string output = "[";
            for (std::size_t index = 0; index < value.array.size(); ++index) {
                auto element = canonical(value.array[index]);
                if (!element) return element;
                if (index != 0) output.push_back(',');
                output += std::move(element).value();
            }
            output.push_back(']');
            return runtime::Result<std::string, PersistenceError>(runtime::Ok(std::move(output)));
        }
        case JsonValue::Kind::Object: {
            std::vector<const std::pair<std::string, JsonValue>*> members;
            members.reserve(value.object.size());
            for (const auto& member : value.object) members.push_back(&member);
            std::sort(members.begin(), members.end(), [](const auto* lhs, const auto* rhs) {
                return lhs->first < rhs->first;
            });
            std::string output = "{";
            for (std::size_t index = 0; index < members.size(); ++index) {
                auto encoded = canonical(members[index]->second);
                if (!encoded) return encoded;
                if (index != 0) output.push_back(',');
                output += quote_json_string(members[index]->first);
                output.push_back(':');
                output += std::move(encoded).value();
            }
            output.push_back('}');
            return runtime::Result<std::string, PersistenceError>(runtime::Ok(std::move(output)));
        }
    }
    return fail<std::string>(PersistenceErrorCode::InvalidJson, value.begin);
}

} // namespace

runtime::Result<std::string, PersistenceError> canonicalize_json(const JsonValue& value) {
    return canonical(value);
}

std::string quote_json_string(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 2);
    detail::append_quoted_json_string(
        value, [&output](std::string_view text) {
            output.append(text);
            return true;
        });
    return output;
}

bool is_valid_utf8(std::string_view value) noexcept {
    std::size_t position = 0;
    while (position < value.size()) {
        const auto first = static_cast<unsigned char>(value[position++]);
        if (first < 0x80)
            continue;
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1;
            codepoint = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2;
            codepoint = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3;
            codepoint = first & 0x07;
        } else {
            return false;
        }
        if (value.size() - position < continuation)
            return false;
        for (std::size_t index = 0; index < continuation; ++index) {
            const auto next = static_cast<unsigned char>(value[position++]);
            if ((next & 0xc0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff)
            return false;
    }
    return true;
}

} // namespace pulp::timeline
