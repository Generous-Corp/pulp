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

class StructuralScanner {
  public:
    StructuralScanner(std::string_view source, const DecodeLimits& limits)
        : source_(source), limits_(limits) {}

    runtime::Result<StructuralPreflightSuccess, PersistenceError> run() {
        std::size_t position = 0;
        Span root;
        if (!skip_value(position, 0, root)) return failed();
        skip_space(position);
        if (position != source_.size()) {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return failed();
        }
        if (!walk_project(root)) return failed();
        return runtime::Result<StructuralPreflightSuccess, PersistenceError>(
            runtime::Ok(StructuralPreflightSuccess{}));
    }

  private:
    struct Span {
        std::size_t begin = 0;
        std::size_t end = 0;
    };

    std::string_view source_;
    const DecodeLimits& limits_;
    PersistenceError error_;
    bool has_error_ = false;
    std::size_t assets_ = 0;
    std::size_t sequences_ = 0;
    std::size_t tracks_ = 0;
    std::size_t clips_ = 0;
    std::size_t notes_ = 0;
    std::size_t locators_ = 0;
    std::size_t representations_ = 0;

    runtime::Result<StructuralPreflightSuccess, PersistenceError> failed() const {
        return runtime::Result<StructuralPreflightSuccess, PersistenceError>(
            runtime::Err(error_));
    }

    void set_error(PersistenceErrorCode code, std::size_t offset,
                   std::uint64_t actual = 0, std::uint64_t limit = 0,
                   std::string path = {}) {
        if (has_error_) return;
        has_error_ = true;
        error_ = PersistenceError{code, offset, actual, limit, std::move(path)};
    }

    void skip_space(std::size_t& position) const noexcept {
        while (position < source_.size()) {
            const auto value = source_[position];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
            ++position;
        }
    }

    static int hex_digit(char value) noexcept {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    bool scan_string(std::size_t& position, std::string* captured = nullptr,
                     std::size_t capture_limit = 128) {
        if (position >= source_.size() || source_[position] != '"') {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return false;
        }
        ++position;
        while (position < source_.size()) {
            const auto byte = static_cast<unsigned char>(source_[position++]);
            if (byte == '"') return true;
            if (byte < 0x20) {
                set_error(PersistenceErrorCode::InvalidJson, position - 1);
                return false;
            }
            if (byte != '\\') {
                if (captured && captured->size() < capture_limit)
                    captured->push_back(static_cast<char>(byte));
                continue;
            }
            if (position >= source_.size()) {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            const auto escape = source_[position++];
            char decoded = '\0';
            bool has_decoded_byte = true;
            switch (escape) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'u': {
                    if (source_.size() - position < 4) {
                        set_error(PersistenceErrorCode::InvalidJson, position);
                        return false;
                    }
                    std::uint32_t codepoint = 0;
                    for (int index = 0; index < 4; ++index) {
                        const auto digit = hex_digit(source_[position++]);
                        if (digit < 0) {
                            set_error(PersistenceErrorCode::InvalidJson, position - 1);
                            return false;
                        }
                        codepoint = codepoint * 16 + static_cast<std::uint32_t>(digit);
                    }
                    if (codepoint <= 0x7f) decoded = static_cast<char>(codepoint);
                    else {
                        has_decoded_byte = false;
                        if (captured) captured->resize(capture_limit);
                    }
                    break;
                }
                default:
                    set_error(PersistenceErrorCode::InvalidJson, position - 1);
                    return false;
            }
            if (captured && has_decoded_byte && captured->size() < capture_limit)
                captured->push_back(decoded);
        }
        set_error(PersistenceErrorCode::InvalidJson, position);
        return false;
    }

    bool skip_value(std::size_t& position, std::size_t depth, Span& span) {
        skip_space(position);
        if (depth > limits_.max_depth) {
            set_error(PersistenceErrorCode::LimitExceeded, position, depth,
                      limits_.max_depth);
            return false;
        }
        span.begin = position;
        if (position >= source_.size()) {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return false;
        }
        const auto first = source_[position];
        if (first == '"') {
            if (!scan_string(position)) return false;
        } else if (first == '{') {
            ++position;
            skip_space(position);
            if (position < source_.size() && source_[position] == '}') {
                ++position;
            } else {
                for (;;) {
                    skip_space(position);
                    if (!scan_string(position)) return false;
                    skip_space(position);
                    if (position >= source_.size() || source_[position++] != ':') {
                        set_error(PersistenceErrorCode::InvalidJson, position);
                        return false;
                    }
                    Span child;
                    if (!skip_value(position, depth + 1, child)) return false;
                    skip_space(position);
                    if (position < source_.size() && source_[position] == '}') {
                        ++position;
                        break;
                    }
                    if (position >= source_.size() || source_[position++] != ',') {
                        set_error(PersistenceErrorCode::InvalidJson, position);
                        return false;
                    }
                }
            }
        } else if (first == '[') {
            ++position;
            skip_space(position);
            if (position < source_.size() && source_[position] == ']') {
                ++position;
            } else {
                for (;;) {
                    Span child;
                    if (!skip_value(position, depth + 1, child)) return false;
                    skip_space(position);
                    if (position < source_.size() && source_[position] == ']') {
                        ++position;
                        break;
                    }
                    if (position >= source_.size() || source_[position++] != ',') {
                        set_error(PersistenceErrorCode::InvalidJson, position);
                        return false;
                    }
                }
            }
        } else if (source_.substr(position, 4) == "true" ||
                   source_.substr(position, 4) == "null") {
            position += 4;
        } else if (source_.substr(position, 5) == "false") {
            position += 5;
        } else {
            if (first == '-') ++position;
            if (position >= source_.size()) {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            if (source_[position] == '0') {
                ++position;
            } else if (source_[position] >= '1' && source_[position] <= '9') {
                while (position < source_.size() && source_[position] >= '0' &&
                       source_[position] <= '9') ++position;
            } else {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            if (position < source_.size() && source_[position] == '.') {
                ++position;
                if (position >= source_.size() || source_[position] < '0' ||
                    source_[position] > '9') {
                    set_error(PersistenceErrorCode::InvalidJson, position);
                    return false;
                }
                while (position < source_.size() && source_[position] >= '0' &&
                       source_[position] <= '9') ++position;
            }
            if (position < source_.size() &&
                (source_[position] == 'e' || source_[position] == 'E')) {
                ++position;
                if (position < source_.size() &&
                    (source_[position] == '+' || source_[position] == '-')) ++position;
                if (position >= source_.size() || source_[position] < '0' ||
                    source_[position] > '9') {
                    set_error(PersistenceErrorCode::InvalidJson, position);
                    return false;
                }
                while (position < source_.size() && source_[position] >= '0' &&
                       source_[position] <= '9') ++position;
            }
        }
        span.end = position;
        return true;
    }

    bool member(Span object, std::string_view wanted, Span& result, bool& found) {
        found = false;
        std::size_t position = object.begin;
        skip_space(position);
        if (position >= object.end || source_[position++] != '{') return true;
        skip_space(position);
        if (position < object.end && source_[position] == '}') return true;
        for (;;) {
            skip_space(position);
            std::string key;
            if (!scan_string(position, &key)) return false;
            skip_space(position);
            if (position >= object.end || source_[position++] != ':') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            Span value;
            if (!skip_value(position, 1, value)) return false;
            if (key == wanted) {
                if (found) {
                    set_error(PersistenceErrorCode::DuplicateKey, object.begin);
                    return false;
                }
                found = true;
                result = value;
            }
            skip_space(position);
            if (position < object.end && source_[position] == '}') break;
            if (position >= object.end || source_[position++] != ',') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
        }
        return true;
    }

    bool string_value(Span span, std::string& value) {
        std::size_t position = span.begin;
        if (!scan_string(position, &value)) return false;
        return position == span.end;
    }

    bool u32_value(Span span, std::uint32_t& value) const noexcept {
        const auto raw = source_.substr(span.begin, span.end - span.begin);
        const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == raw.data() + raw.size();
    }

    bool exact_envelope_keys(Span object, bool& exact) {
        exact = false;
        std::size_t position = object.begin;
        skip_space(position);
        if (position >= object.end || source_[position++] != '{') return true;
        std::size_t members = 0;
        bool data = false;
        bool type = false;
        bool version = false;
        skip_space(position);
        if (position < object.end && source_[position] == '}') return true;
        for (;;) {
            skip_space(position);
            std::string key;
            if (!scan_string(position, &key)) return false;
            skip_space(position);
            if (position >= object.end || source_[position++] != ':') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            Span value;
            if (!skip_value(position, 1, value)) return false;
            ++members;
            if (key == "data") data = true;
            else if (key == "type_name") type = true;
            else if (key == "version") version = true;
            skip_space(position);
            if (position < object.end && source_[position] == '}') break;
            if (position >= object.end || source_[position++] != ',') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
        }
        exact = members == 3 && data && type && version;
        return true;
    }

    bool envelope(Span value, std::string& type, std::uint32_t& version,
                  Span& data, bool& valid_shape) {
        Span type_span;
        Span version_span;
        bool has_type = false;
        bool has_version = false;
        bool has_data = false;
        bool exact_keys = false;
        if (!exact_envelope_keys(value, exact_keys) ||
            !member(value, "type_name", type_span, has_type) ||
            !member(value, "version", version_span, has_version) ||
            !member(value, "data", data, has_data)) return false;
        valid_shape = false;
        if (!has_type) return true;
        if (!string_value(type_span, type)) return false;
        if (!has_version || !has_data) return true;
        valid_shape = exact_keys && u32_value(version_span, version) &&
                      data.begin < data.end &&
                      source_[data.begin] == '{';
        return true;
    }

    bool require_structural_shape(bool valid_shape, std::uint32_t version,
                                  const std::string& path, std::size_t offset) {
        if (valid_shape && version == 1) return true;
        set_error(PersistenceErrorCode::InvalidSchema, offset, 0, 0, path);
        return false;
    }

    enum ValueShape : std::uint8_t {
        StringShape = 1 << 0,
        ObjectShape = 1 << 1,
        ArrayShape = 1 << 2,
        NullShape = 1 << 3,
        NumberShape = 1 << 4,
    };

    bool has_shape(Span value, std::uint8_t allowed) const noexcept {
        if (value.begin >= value.end) return false;
        const auto first = source_[value.begin];
        const auto shape = first == '"' ? StringShape
                           : first == '{' ? ObjectShape
                           : first == '[' ? ArrayShape
                           : first == 'n' ? NullShape
                           : first == '-' || (first >= '0' && first <= '9') ? NumberShape
                                          : 0;
        return (allowed & shape) != 0;
    }

    bool require_member(Span object, std::string_view name, std::uint8_t allowed,
                        const std::string& path) {
        Span value;
        bool found = false;
        if (!member(object, name, value, found)) return false;
        if (found && has_shape(value, allowed)) return true;
        set_error(PersistenceErrorCode::InvalidSchema,
                  found ? value.begin : object.begin, 0, 0,
                  path + "/" + std::string(name));
        return false;
    }

    template <typename ElementFn>
    bool governed_array(Span array, std::size_t& count, std::size_t maximum,
                        const std::string& path, ElementFn&& visit) {
        std::size_t position = array.begin;
        skip_space(position);
        if (position >= array.end || source_[position++] != '[') {
            set_error(PersistenceErrorCode::InvalidSchema, array.begin, 0, 0, path);
            return false;
        }
        skip_space(position);
        if (position < array.end && source_[position] == ']') return true;
        std::size_t index = 0;
        for (;;) {
            skip_space(position);
            if (count >= maximum) {
                set_error(PersistenceErrorCode::LimitExceeded, position, count + 1,
                          maximum, path);
                return false;
            }
            ++count;
            Span element;
            if (!skip_value(position, 1, element) || !visit(element, index)) return false;
            ++index;
            skip_space(position);
            if (position < array.end && source_[position] == ']') return true;
            if (position >= array.end || source_[position++] != ',') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
        }
    }

    bool walk_project(Span value) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.project") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, "", value.begin)) return false;
        if (!require_member(data, "id", StringShape, "/data") ||
            !require_member(data, "name", StringShape, "/data") ||
            !require_member(data, "next_item_id", StringShape, "/data") ||
            !require_member(data, "root_sequence_id", StringShape, "/data")) return false;
        Span assets;
        Span sequences;
        bool has_assets = false;
        bool has_sequences = false;
        if (!member(data, "assets", assets, has_assets) ||
            !member(data, "sequences", sequences, has_sequences)) return false;
        if (!has_assets || !has_sequences) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0, "/data");
            return false;
        }
        if (has_assets && !governed_array(
                assets, assets_, limits_.max_assets, "/data/assets",
                [&](Span element, std::size_t index) {
                    return walk_asset(element, "/data/assets/" + std::to_string(index));
                })) return false;
        return !has_sequences || governed_array(
            sequences, sequences_, limits_.max_sequences, "/data/sequences",
            [&](Span element, std::size_t index) {
                return walk_sequence(element, "/data/sequences/" + std::to_string(index));
            });
    }

    bool walk_asset(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.asset") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin)) return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "content_hash", StringShape, data_path) ||
            !require_member(data, "frame_count", StringShape, data_path) ||
            !require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "name", StringShape, data_path) ||
            !require_member(data, "sample_rate", ObjectShape, data_path) ||
            !require_member(data, "storage_policy", StringShape, data_path)) return false;
        Span locators;
        Span representations;
        bool has_locators = false;
        bool has_representations = false;
        if (!member(data, "locators", locators, has_locators) ||
            !member(data, "representations", representations, has_representations)) return false;
        if (!has_locators || !has_representations) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0, path + "/data");
            return false;
        }
        if (has_locators && !governed_array(
                locators, locators_, limits_.max_locators, path + "/data/locators",
                [](Span, std::size_t) { return true; })) return false;
        return !has_representations || governed_array(
            representations, representations_, limits_.max_representations,
            path + "/data/representations",
            [&](Span element, std::size_t index) {
                return walk_representation(
                    element, path + "/data/representations/" + std::to_string(index));
            });
    }

    bool walk_representation(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.asset_representation") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin)) return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "content_hash", StringShape, data_path) ||
            !require_member(data, "role", StringShape, data_path) ||
            !require_member(data, "storage_policy", StringShape, data_path)) return false;
        Span locators;
        bool found = false;
        if (!member(data, "locators", locators, found)) return false;
        if (!found) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/locators");
            return false;
        }
        return governed_array(
            locators, locators_, limits_.max_locators, path + "/data/locators",
            [](Span, std::size_t) { return true; });
    }

    bool walk_sequence(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.sequence") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin)) return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "absolute_duration", ObjectShape | NullShape, data_path) ||
            !require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "musical_duration", StringShape | NullShape, data_path) ||
            !require_member(data, "name", StringShape, data_path)) return false;
        Span tracks;
        bool found = false;
        if (!member(data, "tracks", tracks, found)) return false;
        if (!found) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/tracks");
            return false;
        }
        return governed_array(
            tracks, tracks_, limits_.max_tracks, path + "/data/tracks",
            [&](Span element, std::size_t index) {
                return walk_track(element,
                                  path + "/data/tracks/" + std::to_string(index));
            });
    }

    bool walk_track(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.track") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin)) return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "name", StringShape, data_path)) return false;
        Span clips;
        bool found = false;
        if (!member(data, "clips", clips, found)) return false;
        if (!found) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/clips");
            return false;
        }
        return governed_array(
            clips, clips_, limits_.max_clips, path + "/data/clips",
            [&](Span element, std::size_t index) {
                return walk_clip(element,
                                 path + "/data/clips/" + std::to_string(index));
            });
    }

    bool walk_clip(Span value, const std::string& path) {
        std::string type;
        std::uint32_t version = 0;
        Span data;
        bool valid_shape = false;
        if (!envelope(value, type, version, data, valid_shape)) return false;
        if (type != "pulp.timeline.clip") {
            set_error(PersistenceErrorCode::InvalidSchema, value.begin, 0, 0, path);
            return false;
        }
        if (!require_structural_shape(valid_shape, version, path, value.begin)) return false;
        const auto data_path = path + "/data";
        if (!require_member(data, "id", StringShape, data_path) ||
            !require_member(data, "time_range", ObjectShape, data_path)) return false;
        Span content;
        bool found = false;
        if (!member(data, "content", content, found)) return false;
        if (!found) {
            set_error(PersistenceErrorCode::InvalidSchema, data.begin, 0, 0,
                      path + "/data/content");
            return false;
        }
        std::string content_type;
        Span content_data;
        if (!envelope(content, content_type, version, content_data, valid_shape)) return false;
        if (!valid_shape) {
            set_error(PersistenceErrorCode::InvalidSchema, content.begin, 0, 0,
                      path + "/data/content");
            return false;
        }
        if (content_type == "pulp.timeline.content.empty" && version == 1) return true;
        if (content_type == "pulp.timeline.content.media" && version == 1) {
            const auto content_path = path + "/data/content/data";
            return require_member(content_data, "asset_id", StringShape, content_path) &&
                   require_member(content_data, "frame_count", StringShape, content_path) &&
                   require_member(content_data, "source_start", StringShape, content_path);
        }
        if (content_type != "pulp.timeline.content.notes" || version != 1) return true;
        Span notes;
        bool has_notes = false;
        if (!member(content_data, "notes", notes, has_notes)) return false;
        if (!has_notes) {
            set_error(PersistenceErrorCode::InvalidSchema, content_data.begin, 0, 0,
                      path + "/data/content/data/notes");
            return false;
        }
        return governed_array(
            notes, notes_, limits_.max_notes, path + "/data/content/data/notes",
            [&](Span note, std::size_t index) {
                const auto note_path = path + "/data/content/data/notes/" +
                                       std::to_string(index);
                if (!has_shape(note, ObjectShape)) {
                    set_error(PersistenceErrorCode::InvalidSchema, note.begin, 0, 0,
                              note_path);
                    return false;
                }
                return require_member(note, "id", StringShape, note_path) &&
                       require_member(note, "start_ticks", StringShape, note_path) &&
                       require_member(note, "duration_ticks", StringShape, note_path) &&
                       require_member(note, "velocity", NumberShape, note_path) &&
                       require_member(note, "pitch", NumberShape, note_path) &&
                       require_member(note, "channel", NumberShape, note_path);
            });
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

runtime::Result<const JsonValue*, PersistenceError>
validate_exact_envelope(const JsonValue& value, std::string_view expected_type,
                        std::uint32_t expected_version, std::string path,
                        PersistenceErrorCode failure_code) {
    if (value.kind != JsonValue::Kind::Object || value.object.size() != 3)
        return fail<const JsonValue*>(failure_code, value.begin, 0, 0,
                                      std::move(path));
    const auto* data = value.find("data");
    const auto* type = value.find("type_name");
    const auto* version = value.find("version");
    if (!data || data->kind != JsonValue::Kind::Object || !type ||
        type->kind != JsonValue::Kind::String || type->scalar != expected_type ||
        !version)
        return fail<const JsonValue*>(failure_code, value.begin, 0, 0,
                                      std::move(path));
    auto decoded_version = parse_u32_number(*version, path + "/version");
    if (!decoded_version || decoded_version.value() != expected_version)
        return fail<const JsonValue*>(failure_code, value.begin, 0, 0,
                                      std::move(path));
    return runtime::Result<const JsonValue*, PersistenceError>(runtime::Ok(data));
}

runtime::Result<StructuralPreflightSuccess, PersistenceError>
preflight_timeline_structure(std::string_view json, const DecodeLimits& limits) {
    if (json.size() > limits.max_input_bytes)
        return fail<StructuralPreflightSuccess>(PersistenceErrorCode::LimitExceeded, 0,
                                                json.size(), limits.max_input_bytes);
    return StructuralScanner(json, limits).run();
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
