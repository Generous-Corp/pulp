#include "schema_json_validation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pulp::timeline::detail {
namespace {

class JsonValidator {
  public:
    JsonValidator(std::string_view source, const DecodeLimits& limits)
        : source_(source), limits_(limits) {}

    std::optional<PersistenceError> run() {
        if (source_.size() > limits_.max_input_bytes)
            return PersistenceError{PersistenceErrorCode::LimitExceeded, 0, source_.size(),
                                    limits_.max_input_bytes};
        if (!value(0))
            return error_;
        space();
        if (position_ != source_.size())
            fail(PersistenceErrorCode::InvalidJson, position_);
        return error_;
    }

  private:
    std::string_view source_;
    const DecodeLimits& limits_;
    std::size_t position_ = 0;
    std::size_t total_values_ = 0;
    std::optional<PersistenceError> error_;

    char peek() const noexcept {
        return position_ < source_.size() ? source_[position_] : '\0';
    }

    void space() noexcept {
        while (position_ < source_.size()) {
            const auto byte = source_[position_];
            if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
                break;
            ++position_;
        }
    }

    bool fail(PersistenceErrorCode code, std::size_t offset, std::uint64_t actual = 0,
              std::uint64_t limit = 0) {
        if (!error_)
            error_ = PersistenceError{code, offset, actual, limit};
        return false;
    }

    bool value(std::size_t depth) {
        space();
        if (depth > limits_.max_depth)
            return fail(PersistenceErrorCode::LimitExceeded, position_, depth, limits_.max_depth);
        if (++total_values_ > limits_.max_total_values)
            return fail(PersistenceErrorCode::LimitExceeded, position_, total_values_,
                        limits_.max_total_values);
        switch (peek()) {
        case '{':
            return object(depth);
        case '[':
            return array(depth);
        case '"':
            return string(nullptr);
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

    bool literal(std::string_view expected) {
        if (source_.substr(position_, expected.size()) != expected)
            return fail(PersistenceErrorCode::InvalidJson, position_);
        position_ += expected.size();
        return true;
    }

    bool number() {
        if (peek() == '-')
            ++position_;
        if (peek() == '0') {
            ++position_;
            if (peek() >= '0' && peek() <= '9')
                return fail(PersistenceErrorCode::InvalidJson, position_);
        } else {
            if (peek() < '1' || peek() > '9')
                return fail(PersistenceErrorCode::InvalidJson, position_);
            while (peek() >= '0' && peek() <= '9')
                ++position_;
        }
        if (peek() == '.') {
            ++position_;
            if (peek() < '0' || peek() > '9')
                return fail(PersistenceErrorCode::InvalidJson, position_);
            while (peek() >= '0' && peek() <= '9')
                ++position_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++position_;
            if (peek() == '+' || peek() == '-')
                ++position_;
            if (peek() < '0' || peek() > '9')
                return fail(PersistenceErrorCode::InvalidJson, position_);
            while (peek() >= '0' && peek() <= '9')
                ++position_;
        }
        return true;
    }

    static int hex(char value) noexcept {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    }

    bool quad(std::uint32_t& output) {
        if (source_.size() - position_ < 4)
            return fail(PersistenceErrorCode::InvalidJson, position_);
        output = 0;
        for (int index = 0; index < 4; ++index) {
            const auto digit = hex(source_[position_++]);
            if (digit < 0)
                return fail(PersistenceErrorCode::InvalidJson, position_ - 1);
            output = output * 16 + static_cast<std::uint32_t>(digit);
        }
        return true;
    }

    bool append_codepoint(std::uint32_t codepoint, std::string* output, std::size_t& decoded_bytes,
                          std::size_t start) {
        const auto bytes = codepoint <= 0x7f     ? 1u
                           : codepoint <= 0x7ff  ? 2u
                           : codepoint <= 0xffff ? 3u
                                                 : 4u;
        decoded_bytes += bytes;
        if (decoded_bytes > limits_.max_string_bytes)
            return fail(PersistenceErrorCode::LimitExceeded, start, decoded_bytes,
                        limits_.max_string_bytes);
        if (!output)
            return true;
        if (bytes == 1) {
            output->push_back(static_cast<char>(codepoint));
        } else if (bytes == 2) {
            output->push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (bytes == 3) {
            output->push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            output->push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
        return true;
    }

    bool string(std::string* output) {
        const auto start = position_;
        if (peek() != '"')
            return fail(PersistenceErrorCode::InvalidJson, position_);
        ++position_;
        std::size_t decoded_bytes = 0;
        if (output)
            output->clear();
        while (position_ < source_.size()) {
            const auto byte = static_cast<unsigned char>(source_[position_++]);
            if (byte == '"')
                return true;
            if (byte < 0x20)
                return fail(PersistenceErrorCode::InvalidJson, position_ - 1);
            if (byte == '\\') {
                if (position_ >= source_.size())
                    return fail(PersistenceErrorCode::InvalidJson, position_);
                const auto escape = source_[position_++];
                char decoded = '\0';
                switch (escape) {
                case '"':
                    decoded = '"';
                    break;
                case '\\':
                    decoded = '\\';
                    break;
                case '/':
                    decoded = '/';
                    break;
                case 'b':
                    decoded = '\b';
                    break;
                case 'f':
                    decoded = '\f';
                    break;
                case 'n':
                    decoded = '\n';
                    break;
                case 'r':
                    decoded = '\r';
                    break;
                case 't':
                    decoded = '\t';
                    break;
                case 'u': {
                    std::uint32_t first = 0;
                    if (!quad(first))
                        return false;
                    auto codepoint = first;
                    if (first >= 0xd800 && first <= 0xdbff) {
                        if (source_.size() - position_ < 6 || source_[position_] != '\\' ||
                            source_[position_ + 1] != 'u')
                            return fail(PersistenceErrorCode::InvalidUtf8, position_);
                        position_ += 2;
                        std::uint32_t second = 0;
                        if (!quad(second) || second < 0xdc00 || second > 0xdfff) {
                            error_ = PersistenceError{PersistenceErrorCode::InvalidUtf8, position_};
                            return false;
                        }
                        codepoint = 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
                    } else if (first >= 0xdc00 && first <= 0xdfff) {
                        return fail(PersistenceErrorCode::InvalidUtf8, position_);
                    }
                    if (!append_codepoint(codepoint, output, decoded_bytes, start))
                        return false;
                    continue;
                }
                default:
                    return fail(PersistenceErrorCode::InvalidJson, position_ - 1);
                }
                if (++decoded_bytes > limits_.max_string_bytes)
                    return fail(PersistenceErrorCode::LimitExceeded, start, decoded_bytes,
                                limits_.max_string_bytes);
                if (output)
                    output->push_back(decoded);
                continue;
            }
            if (byte < 0x80) {
                if (++decoded_bytes > limits_.max_string_bytes)
                    return fail(PersistenceErrorCode::LimitExceeded, start, decoded_bytes,
                                limits_.max_string_bytes);
                if (output)
                    output->push_back(static_cast<char>(byte));
                continue;
            }
            std::size_t continuation = 0;
            std::uint32_t codepoint = 0;
            if (byte >= 0xc2 && byte <= 0xdf) {
                continuation = 1;
                codepoint = byte & 0x1f;
            } else if (byte >= 0xe0 && byte <= 0xef) {
                continuation = 2;
                codepoint = byte & 0x0f;
            } else if (byte >= 0xf0 && byte <= 0xf4) {
                continuation = 3;
                codepoint = byte & 0x07;
            } else {
                return fail(PersistenceErrorCode::InvalidUtf8, position_ - 1);
            }
            if (source_.size() - position_ < continuation)
                return fail(PersistenceErrorCode::InvalidUtf8, position_);
            if (decoded_bytes + continuation + 1 > limits_.max_string_bytes)
                return fail(PersistenceErrorCode::LimitExceeded, start,
                            decoded_bytes + continuation + 1, limits_.max_string_bytes);
            decoded_bytes += continuation + 1;
            if (output)
                output->push_back(static_cast<char>(byte));
            for (std::size_t index = 0; index < continuation; ++index) {
                const auto next = static_cast<unsigned char>(source_[position_++]);
                if ((next & 0xc0) != 0x80)
                    return fail(PersistenceErrorCode::InvalidUtf8, position_ - 1);
                if (output)
                    output->push_back(static_cast<char>(next));
                codepoint = (codepoint << 6) | (next & 0x3f);
            }
            if ((continuation == 2 && codepoint < 0x800) ||
                (continuation == 3 && codepoint < 0x10000) ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff)
                return fail(PersistenceErrorCode::InvalidUtf8, position_);
        }
        return fail(PersistenceErrorCode::InvalidJson, position_);
    }

    bool array(std::size_t depth) {
        ++position_;
        space();
        if (peek() == ']') {
            ++position_;
            return true;
        }
        std::size_t elements = 0;
        for (;;) {
            if (++elements > limits_.max_array_elements)
                return fail(PersistenceErrorCode::LimitExceeded, position_, elements,
                            limits_.max_array_elements);
            if (!value(depth + 1))
                return false;
            space();
            if (peek() == ']') {
                ++position_;
                return true;
            }
            if (peek() != ',')
                return fail(PersistenceErrorCode::InvalidJson, position_);
            ++position_;
        }
    }

    bool object(std::size_t depth) {
        const auto begin = position_++;
        space();
        if (peek() == '}') {
            ++position_;
            return true;
        }
        constexpr std::size_t inline_key_capacity = 16;
        std::array<std::string, inline_key_capacity> inline_keys;
        std::size_t inline_key_count = 0;
        std::vector<std::string> overflow_keys;
        const auto key_count = [&] {
            return overflow_keys.empty() ? inline_key_count : overflow_keys.size();
        };
        const auto remember_key = [&](std::string key) {
            if (inline_key_count < inline_key_capacity) {
                inline_keys[inline_key_count++] = std::move(key);
                return;
            }
            if (overflow_keys.empty()) {
                overflow_keys.reserve(
                    std::min(limits_.max_object_members, inline_key_capacity * 2));
                for (auto& inline_key : inline_keys)
                    overflow_keys.push_back(std::move(inline_key));
            }
            overflow_keys.push_back(std::move(key));
        };
        const auto duplicate_key = [&] {
            if (!overflow_keys.empty()) {
                std::sort(overflow_keys.begin(), overflow_keys.end());
                return std::adjacent_find(overflow_keys.begin(), overflow_keys.end()) !=
                       overflow_keys.end();
            }
            std::sort(inline_keys.begin(), inline_keys.begin() + inline_key_count);
            return std::adjacent_find(inline_keys.begin(),
                                      inline_keys.begin() + inline_key_count) !=
                   inline_keys.begin() + inline_key_count;
        };
        for (;;) {
            if (key_count() >= limits_.max_object_members)
                return fail(PersistenceErrorCode::LimitExceeded, position_, key_count() + 1,
                            limits_.max_object_members);
            space();
            std::string key;
            if (!string(&key))
                return false;
            remember_key(std::move(key));
            space();
            if (peek() != ':')
                return fail(PersistenceErrorCode::InvalidJson, position_);
            ++position_;
            if (!value(depth + 1))
                return false;
            space();
            if (peek() == '}') {
                ++position_;
                if (duplicate_key())
                    return fail(PersistenceErrorCode::DuplicateKey, begin);
                return true;
            }
            if (peek() != ',')
                return fail(PersistenceErrorCode::InvalidJson, position_);
            ++position_;
        }
    }
};

} // namespace

std::optional<PersistenceError> validate_json_syntax_and_limits(std::string_view source,
                                                                const DecodeLimits& limits) {
    return JsonValidator(source, limits).run();
}

} // namespace pulp::timeline::detail
