#include <pulp/timeline/schema_json.hpp>

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

class Parser {
  public:
    Parser(std::shared_ptr<const std::string> source, const DecodeLimits& limits)
        : source_(std::move(source)), limits_(limits) {}

    runtime::Result<JsonValue, PersistenceError> parse() {
        auto result = parse_value(0);
        if (!result)
            return result;
        skip_space();
        if (position_ != source_->size())
            return fail<JsonValue>(PersistenceErrorCode::InvalidJson, position_);
        return result;
    }

  private:
    std::shared_ptr<const std::string> source_;
    const DecodeLimits& limits_;
    std::size_t position_ = 0;
    std::size_t total_values_ = 0;

    char peek() const noexcept {
        return position_ < source_->size() ? (*source_)[position_] : '\0';
    }

    void skip_space() noexcept {
        while (position_ < source_->size()) {
            const auto value = (*source_)[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
                break;
            ++position_;
        }
    }

    runtime::Result<JsonValue, PersistenceError> parse_value(std::size_t depth) {
        skip_space();
        if (depth > limits_.max_depth)
            return fail<JsonValue>(PersistenceErrorCode::LimitExceeded, position_, depth,
                                   limits_.max_depth);
        if (++total_values_ > limits_.max_total_values)
            return fail<JsonValue>(PersistenceErrorCode::LimitExceeded, position_, total_values_,
                                   limits_.max_total_values);
        const auto begin = position_;
        switch (peek()) {
            case '{': return parse_object(depth, begin);
            case '[': return parse_array(depth, begin);
            case '"': {
                auto string = parse_string();
                if (!string)
                    return fail<JsonValue>(string.error().code, string.error().byte_offset,
                                           string.error().actual, string.error().limit);
                return runtime::Result<JsonValue, PersistenceError>(runtime::Ok(
                    JsonValue{JsonValue::Kind::String, false, std::move(string).value(), {}, {},
                              begin, position_}));
            }
            case 't': return parse_literal("true", JsonValue::Kind::Boolean, true, begin);
            case 'f': return parse_literal("false", JsonValue::Kind::Boolean, false, begin);
            case 'n': return parse_literal("null", JsonValue::Kind::Null, false, begin);
            default: return parse_number(begin);
        }
    }

    runtime::Result<JsonValue, PersistenceError> parse_literal(std::string_view literal,
                                                                JsonValue::Kind kind,
                                                                bool boolean,
                                                                std::size_t begin) {
        if (source_->compare(position_, literal.size(), literal) != 0)
            return fail<JsonValue>(PersistenceErrorCode::InvalidJson, position_);
        position_ += literal.size();
        return runtime::Result<JsonValue, PersistenceError>(runtime::Ok(
            JsonValue{kind, boolean, {}, {}, {}, begin, position_}));
    }

    runtime::Result<JsonValue, PersistenceError> parse_number(std::size_t begin) {
        if (peek() == '-')
            ++position_;
        if (peek() == '0') {
            ++position_;
            if (peek() >= '0' && peek() <= '9')
                return fail<JsonValue>(PersistenceErrorCode::InvalidJson, position_);
        } else {
            if (peek() < '1' || peek() > '9')
                return fail<JsonValue>(PersistenceErrorCode::InvalidJson, position_);
            while (peek() >= '0' && peek() <= '9')
                ++position_;
        }
        if (peek() == '.') {
            ++position_;
            if (peek() < '0' || peek() > '9')
                return fail<JsonValue>(PersistenceErrorCode::InvalidJson, position_);
            while (peek() >= '0' && peek() <= '9')
                ++position_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++position_;
            if (peek() == '+' || peek() == '-')
                ++position_;
            if (peek() < '0' || peek() > '9')
                return fail<JsonValue>(PersistenceErrorCode::InvalidJson, position_);
            while (peek() >= '0' && peek() <= '9')
                ++position_;
        }
        return runtime::Result<JsonValue, PersistenceError>(runtime::Ok(JsonValue{
            JsonValue::Kind::Number, false, source_->substr(begin, position_ - begin), {}, {},
            begin, position_}));
    }

    static int hex_digit(char value) noexcept {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    runtime::Result<std::uint32_t, PersistenceError> parse_hex_quad() {
        if (source_->size() - position_ < 4)
            return fail<std::uint32_t>(PersistenceErrorCode::InvalidJson, position_);
        std::uint32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            const auto digit = hex_digit((*source_)[position_++]);
            if (digit < 0)
                return fail<std::uint32_t>(PersistenceErrorCode::InvalidJson, position_ - 1);
            result = result * 16 + static_cast<std::uint32_t>(digit);
        }
        return runtime::Result<std::uint32_t, PersistenceError>(runtime::Ok(result));
    }

    static void append_codepoint(std::string& output, std::uint32_t value) {
        if (value <= 0x7f) {
            output.push_back(static_cast<char>(value));
        } else if (value <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (value >> 6)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else if (value <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (value >> 12)));
            output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (value >> 18)));
            output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
    }

    runtime::Result<std::string, PersistenceError> parse_string() {
        const auto start = position_;
        if (peek() != '"')
            return fail<std::string>(PersistenceErrorCode::InvalidJson, position_);
        ++position_;
        std::string result;
        while (position_ < source_->size()) {
            const auto byte = static_cast<unsigned char>((*source_)[position_++]);
            if (byte == '"') {
                if (result.size() > limits_.max_string_bytes)
                    return fail<std::string>(PersistenceErrorCode::LimitExceeded, start,
                                             result.size(), limits_.max_string_bytes);
                return runtime::Result<std::string, PersistenceError>(runtime::Ok(std::move(result)));
            }
            if (byte < 0x20)
                return fail<std::string>(PersistenceErrorCode::InvalidJson, position_ - 1);
            if (byte == '\\') {
                if (position_ >= source_->size())
                    return fail<std::string>(PersistenceErrorCode::InvalidJson, position_);
                const auto escape = (*source_)[position_++];
                switch (escape) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u': {
                        auto first = parse_hex_quad();
                        if (!first)
                            return fail<std::string>(first.error().code, first.error().byte_offset);
                        auto codepoint = first.value();
                        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                            if (source_->size() - position_ < 6 || (*source_)[position_] != '\\' ||
                                (*source_)[position_ + 1] != 'u')
                                return fail<std::string>(PersistenceErrorCode::InvalidUtf8, position_);
                            position_ += 2;
                            auto second = parse_hex_quad();
                            if (!second || second.value() < 0xdc00 || second.value() > 0xdfff)
                                return fail<std::string>(PersistenceErrorCode::InvalidUtf8, position_);
                            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
                                        (second.value() - 0xdc00);
                        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                            return fail<std::string>(PersistenceErrorCode::InvalidUtf8, position_);
                        }
                        append_codepoint(result, codepoint);
                        break;
                    }
                    default: return fail<std::string>(PersistenceErrorCode::InvalidJson,
                                                       position_ - 1);
                }
                if (result.size() > limits_.max_string_bytes)
                    return fail<std::string>(PersistenceErrorCode::LimitExceeded, start,
                                             result.size(), limits_.max_string_bytes);
                continue;
            }
            if (byte < 0x80) {
                result.push_back(static_cast<char>(byte));
                if (result.size() > limits_.max_string_bytes)
                    return fail<std::string>(PersistenceErrorCode::LimitExceeded, start,
                                             result.size(), limits_.max_string_bytes);
                continue;
            }
            std::size_t continuation = 0;
            std::uint32_t codepoint = 0;
            if (byte >= 0xc2 && byte <= 0xdf) {
                continuation = 1; codepoint = byte & 0x1f;
            } else if (byte >= 0xe0 && byte <= 0xef) {
                continuation = 2; codepoint = byte & 0x0f;
            } else if (byte >= 0xf0 && byte <= 0xf4) {
                continuation = 3; codepoint = byte & 0x07;
            } else {
                return fail<std::string>(PersistenceErrorCode::InvalidUtf8, position_ - 1);
            }
            if (source_->size() - position_ < continuation)
                return fail<std::string>(PersistenceErrorCode::InvalidUtf8, position_);
            result.push_back(static_cast<char>(byte));
            for (std::size_t index = 0; index < continuation; ++index) {
                const auto next = static_cast<unsigned char>((*source_)[position_++]);
                if ((next & 0xc0) != 0x80)
                    return fail<std::string>(PersistenceErrorCode::InvalidUtf8, position_ - 1);
                result.push_back(static_cast<char>(next));
                codepoint = (codepoint << 6) | (next & 0x3f);
            }
            if (result.size() > limits_.max_string_bytes)
                return fail<std::string>(PersistenceErrorCode::LimitExceeded, start,
                                         result.size(), limits_.max_string_bytes);
            if ((continuation == 2 && codepoint < 0x800) ||
                (continuation == 3 && codepoint < 0x10000) ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff)
                return fail<std::string>(PersistenceErrorCode::InvalidUtf8, position_);
        }
        return fail<std::string>(PersistenceErrorCode::InvalidJson, position_);
    }

    runtime::Result<JsonValue, PersistenceError> parse_array(std::size_t depth,
                                                              std::size_t begin) {
        ++position_;
        JsonValue result{JsonValue::Kind::Array, false, {}, {}, {}, begin, 0};
        skip_space();
        if (peek() == ']') {
            ++position_; result.end = position_;
            return runtime::Result<JsonValue, PersistenceError>(runtime::Ok(std::move(result)));
        }
        for (;;) {
            if (result.array.size() >= limits_.max_array_elements)
                return fail<JsonValue>(PersistenceErrorCode::LimitExceeded, position_,
                                       result.array.size() + 1, limits_.max_array_elements);
            auto value = parse_value(depth + 1);
            if (!value)
                return value;
            result.array.push_back(std::move(value).value());
            skip_space();
            if (peek() == ']') {
                ++position_; result.end = position_;
                return runtime::Result<JsonValue, PersistenceError>(runtime::Ok(std::move(result)));
            }
            if (peek() != ',')
                return fail<JsonValue>(PersistenceErrorCode::InvalidJson, position_);
            ++position_;
        }
    }

    runtime::Result<JsonValue, PersistenceError> parse_object(std::size_t depth,
                                                               std::size_t begin) {
        ++position_;
        JsonValue result{JsonValue::Kind::Object, false, {}, {}, {}, begin, 0};
        skip_space();
        if (peek() == '}') {
            ++position_; result.end = position_;
            return runtime::Result<JsonValue, PersistenceError>(runtime::Ok(std::move(result)));
        }
        for (;;) {
            if (result.object.size() >= limits_.max_object_members)
                return fail<JsonValue>(PersistenceErrorCode::LimitExceeded, position_,
                                       result.object.size() + 1, limits_.max_object_members);
            skip_space();
            auto key = parse_string();
            if (!key)
                return fail<JsonValue>(key.error().code, key.error().byte_offset,
                                       key.error().actual, key.error().limit);
            skip_space();
            if (peek() != ':')
                return fail<JsonValue>(PersistenceErrorCode::InvalidJson, position_);
            ++position_;
            auto value = parse_value(depth + 1);
            if (!value)
                return value;
            result.object.emplace_back(std::move(key).value(), std::move(value).value());
            skip_space();
            if (peek() == '}') {
                ++position_;
                result.end = position_;
                std::vector<std::string_view> keys;
                keys.reserve(result.object.size());
                for (const auto& member : result.object)
                    keys.push_back(member.first);
                std::sort(keys.begin(), keys.end());
                if (std::adjacent_find(keys.begin(), keys.end()) != keys.end())
                    return fail<JsonValue>(PersistenceErrorCode::DuplicateKey, begin);
                return runtime::Result<JsonValue, PersistenceError>(runtime::Ok(std::move(result)));
            }
            if (peek() != ',')
                return fail<JsonValue>(PersistenceErrorCode::InvalidJson, position_);
            ++position_;
        }
    }
};

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

template <typename T>
runtime::Result<T, PersistenceError> parse_wide(const JsonValue& value, std::string path,
                                                bool allow_negative) {
    if (value.kind != JsonValue::Kind::String || value.scalar.empty())
        return fail<T>(PersistenceErrorCode::UnexpectedType, value.begin, 0, 0, std::move(path));
    const auto text = std::string_view(value.scalar);
    if (text == "-0" || text.front() == '+' ||
        (text.front() == '0' && text.size() != 1) ||
        (text.front() == '-' && (!allow_negative || text.size() == 1 ||
                                 (text.size() > 2 && text[1] == '0'))))
        return fail<T>(PersistenceErrorCode::InvalidNumber, value.begin, 0, 0, std::move(path));
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return fail<T>(PersistenceErrorCode::InvalidNumber, value.begin, 0, 0, std::move(path));
    return runtime::Result<T, PersistenceError>(runtime::Ok(result));
}

} // namespace

DecodeLimits DecodeLimits::web_defaults() noexcept {
    auto limits = DecodeLimits{};
    limits.max_input_bytes = 256ull * 1024ull * 1024ull;
    limits.max_total_values = 8'000'000;
    limits.max_array_elements = 2'000'000;
    limits.max_opaque_bytes = 16ull * 1024ull * 1024ull;
    limits.max_notes = 1'000'000;
    return limits;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (kind != Kind::Object) return nullptr;
    for (const auto& member : object)
        if (member.first == key) return &member.second;
    return nullptr;
}

std::string_view ParsedJson::raw(const JsonValue& value) const noexcept {
    if (value.begin > value.end || value.end > source_->size()) return {};
    return std::string_view(*source_).substr(value.begin, value.end - value.begin);
}

runtime::Result<std::shared_ptr<const ParsedJson>, PersistenceError>
parse_json(std::string_view json, const DecodeLimits& limits) {
    if (json.size() > limits.max_input_bytes)
        return fail<std::shared_ptr<const ParsedJson>>(PersistenceErrorCode::LimitExceeded, 0,
                                                       json.size(), limits.max_input_bytes);
    auto source = std::make_shared<const std::string>(json);
    Parser parser(source, limits);
    auto root = parser.parse();
    if (!root)
        return fail<std::shared_ptr<const ParsedJson>>(root.error().code, root.error().byte_offset,
                                                       root.error().actual, root.error().limit,
                                                       root.error().path);
    auto parsed = std::make_shared<ParsedJson>();
    parsed->source_ = std::move(source);
    parsed->root_ = std::move(root).value();
    return runtime::Result<std::shared_ptr<const ParsedJson>, PersistenceError>(
        runtime::Ok(std::shared_ptr<const ParsedJson>(std::move(parsed))));
}

runtime::Result<std::string, PersistenceError> canonicalize_json(const JsonValue& value) {
    return canonical(value);
}

std::string quote_json_string(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output = "\"";
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (byte < 0x20) {
                    output += "\\u00";
                    output.push_back(digits[byte >> 4]);
                    output.push_back(digits[byte & 0x0f]);
                } else {
                    output.push_back(raw);
                }
        }
    }
    output.push_back('"');
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

runtime::Result<std::uint64_t, PersistenceError>
parse_canonical_u64_string(const JsonValue& value, std::string path) {
    return parse_wide<std::uint64_t>(value, std::move(path), false);
}

runtime::Result<std::int64_t, PersistenceError>
parse_canonical_i64_string(const JsonValue& value, std::string path) {
    return parse_wide<std::int64_t>(value, std::move(path), true);
}

runtime::Result<std::uint32_t, PersistenceError>
parse_u32_number(const JsonValue& value, std::string path) {
    if (value.kind != JsonValue::Kind::Number || value.scalar.empty() || value.scalar.front() == '-' ||
        value.scalar.find_first_of(".eE") != std::string::npos)
        return fail<std::uint32_t>(PersistenceErrorCode::UnexpectedType, value.begin, 0, 0,
                                   std::move(path));
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.scalar.data(),
                                        value.scalar.data() + value.scalar.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.scalar.data() + value.scalar.size())
        return fail<std::uint32_t>(PersistenceErrorCode::InvalidNumber, value.begin, 0, 0,
                                   std::move(path));
    return runtime::Result<std::uint32_t, PersistenceError>(runtime::Ok(result));
}

} // namespace pulp::timeline
