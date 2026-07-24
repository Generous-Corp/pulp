#include "json_span_reader.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

namespace pulp::timeline::detail {

JsonSpanReader::JsonSpanReader(std::string_view source, const DecodeLimits& limits) noexcept
    : source_(source), limits_(limits) {}

void JsonSpanReader::set_error(PersistenceErrorCode code, std::size_t offset, std::uint64_t actual,
                               std::uint64_t limit, std::string path) {
    if (!has_error_) {
        has_error_ = true;
        error_ = PersistenceError{code, offset, actual, limit, std::move(path)};
    }
}

void JsonSpanReader::skip_space(std::size_t& position) const noexcept {
    while (position < source_.size()) {
        const auto value = source_[position];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
            break;
        ++position;
    }
}

int JsonSpanReader::hex_digit(char value) noexcept {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool JsonSpanReader::scan_string(std::size_t& position, std::string* captured,
                                 std::size_t capture_limit) {
    if (position >= source_.size() || source_[position] != '"') {
        set_error(PersistenceErrorCode::InvalidJson, position);
        return false;
    }
    ++position;
    while (position < source_.size()) {
        const auto byte = static_cast<unsigned char>(source_[position++]);
        if (byte == '"')
            return true;
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
            if (codepoint <= 0x7f)
                decoded = static_cast<char>(codepoint);
            else {
                has_decoded_byte = false;
                if (captured)
                    captured->resize(capture_limit);
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

bool JsonSpanReader::skip_value(std::size_t& position, std::size_t depth, JsonSpan& span) {
    skip_space(position);
    if (depth > limits_.max_depth) {
        set_error(PersistenceErrorCode::LimitExceeded, position, depth, limits_.max_depth);
        return false;
    }
    span.begin = position;
    if (position >= source_.size()) {
        set_error(PersistenceErrorCode::InvalidJson, position);
        return false;
    }
    const auto first = source_[position];
    if (first == '"') {
        if (!scan_string(position))
            return false;
    } else if (first == '{') {
        ++position;
        skip_space(position);
        if (position < source_.size() && source_[position] == '}') {
            ++position;
        } else {
            for (;;) {
                skip_space(position);
                if (!scan_string(position))
                    return false;
                skip_space(position);
                if (position >= source_.size() || source_[position++] != ':') {
                    set_error(PersistenceErrorCode::InvalidJson, position);
                    return false;
                }
                JsonSpan child;
                if (!skip_value(position, depth + 1, child))
                    return false;
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
                JsonSpan child;
                if (!skip_value(position, depth + 1, child))
                    return false;
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
    } else if (source_.substr(position, 4) == "true" || source_.substr(position, 4) == "null") {
        position += 4;
    } else if (source_.substr(position, 5) == "false") {
        position += 5;
    } else {
        if (first == '-')
            ++position;
        if (position >= source_.size()) {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return false;
        }
        if (source_[position] == '0') {
            ++position;
        } else if (source_[position] >= '1' && source_[position] <= '9') {
            while (position < source_.size() && source_[position] >= '0' &&
                   source_[position] <= '9')
                ++position;
        } else {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return false;
        }
        if (position < source_.size() && source_[position] == '.') {
            ++position;
            if (position >= source_.size() || source_[position] < '0' || source_[position] > '9') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            while (position < source_.size() && source_[position] >= '0' &&
                   source_[position] <= '9')
                ++position;
        }
        if (position < source_.size() && (source_[position] == 'e' || source_[position] == 'E')) {
            ++position;
            if (position < source_.size() && (source_[position] == '+' || source_[position] == '-'))
                ++position;
            if (position >= source_.size() || source_[position] < '0' || source_[position] > '9') {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            while (position < source_.size() && source_[position] >= '0' &&
                   source_[position] <= '9')
                ++position;
        }
    }
    span.end = position;
    return true;
}

bool JsonSpanReader::member(JsonSpan object, std::string_view wanted, JsonSpan& result,
                            bool& found) {
    std::array requested{JsonSpanMember{wanted}};
    if (!members(object, requested))
        return false;
    result = requested.front().span;
    found = requested.front().found;
    return true;
}

bool JsonSpanReader::members(JsonSpan object, std::span<JsonSpanMember> wanted,
                             std::size_t* member_count) {
    for (auto& request : wanted) {
        request.span = {};
        request.found = false;
    }
    if (member_count)
        *member_count = 0;
    std::size_t position = object.begin;
    skip_space(position);
    if (position >= object.end || source_[position++] != '{')
        return true;
    skip_space(position);
    if (position < object.end && source_[position] == '}')
        return true;
    for (;;) {
        skip_space(position);
        std::string key;
        if (!scan_string(position, &key))
            return false;
        skip_space(position);
        if (position >= object.end || source_[position++] != ':') {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return false;
        }
        JsonSpan value;
        if (!skip_value(position, 1, value))
            return false;
        if (member_count)
            ++*member_count;
        for (auto& request : wanted) {
            if (key == request.name) {
                if (request.found) {
                    set_error(PersistenceErrorCode::DuplicateKey, object.begin);
                    return false;
                }
                request.found = true;
                request.span = value;
            }
        }
        skip_space(position);
        if (position < object.end && source_[position] == '}')
            break;
        if (position >= object.end || source_[position++] != ',') {
            set_error(PersistenceErrorCode::InvalidJson, position);
            return false;
        }
    }
    return true;
}

bool JsonSpanReader::string_value(JsonSpan span, std::string& value) {
    value.clear();
    std::size_t position = span.begin;
    return scan_string(position, &value) && position == span.end;
}

bool JsonSpanReader::quad(std::size_t& position, std::uint32_t& output) {
    if (source_.size() - position < 4) {
        set_error(PersistenceErrorCode::InvalidJson, position);
        return false;
    }
    output = 0;
    for (int index = 0; index < 4; ++index) {
        const auto digit = hex_digit(source_[position++]);
        if (digit < 0) {
            set_error(PersistenceErrorCode::InvalidJson, position - 1);
            return false;
        }
        output = output * 16 + static_cast<std::uint32_t>(digit);
    }
    return true;
}

bool JsonSpanReader::append_codepoint(std::uint32_t codepoint, std::string& output,
                                      std::size_t& decoded_bytes, std::size_t start) {
    const auto bytes = codepoint <= 0x7f     ? 1u
                       : codepoint <= 0x7ff  ? 2u
                       : codepoint <= 0xffff ? 3u
                                             : 4u;
    decoded_bytes += bytes;
    if (decoded_bytes > limits_.max_string_bytes) {
        set_error(PersistenceErrorCode::LimitExceeded, start, decoded_bytes,
                  limits_.max_string_bytes);
        return false;
    }
    if (bytes == 1)
        output.push_back(static_cast<char>(codepoint));
    else if (bytes == 2) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (bytes == 3) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    return true;
}

bool JsonSpanReader::decode_string(std::size_t& position, std::string& output) {
    const auto start = position;
    if (position >= source_.size() || source_[position++] != '"')
        return false;
    output.clear();
    std::size_t decoded_bytes = 0;
    while (position < source_.size()) {
        const auto byte = static_cast<unsigned char>(source_[position++]);
        if (byte == '"')
            return true;
        if (byte < 0x20) {
            set_error(PersistenceErrorCode::InvalidJson, position - 1);
            return false;
        }
        if (byte == '\\') {
            if (position >= source_.size()) {
                set_error(PersistenceErrorCode::InvalidJson, position);
                return false;
            }
            const auto escape = source_[position++];
            if (escape == 'u') {
                std::uint32_t first = 0;
                if (!quad(position, first))
                    return false;
                auto codepoint = first;
                if (first >= 0xd800 && first <= 0xdbff) {
                    if (source_.size() - position < 6 || source_[position] != '\\' ||
                        source_[position + 1] != 'u') {
                        set_error(PersistenceErrorCode::InvalidUtf8, position);
                        return false;
                    }
                    position += 2;
                    std::uint32_t second = 0;
                    if (!quad(position, second) || second < 0xdc00 || second > 0xdfff) {
                        has_error_ = true;
                        error_ =
                            PersistenceError{PersistenceErrorCode::InvalidUtf8, position};
                        return false;
                    }
                    codepoint = 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
                } else if (first >= 0xdc00 && first <= 0xdfff) {
                    set_error(PersistenceErrorCode::InvalidUtf8, position);
                    return false;
                }
                if (!append_codepoint(codepoint, output, decoded_bytes, start))
                    return false;
                continue;
            }
            const char decoded = escape == '"'    ? '"'
                                 : escape == '\\' ? '\\'
                                 : escape == '/'  ? '/'
                                 : escape == 'b'  ? '\b'
                                 : escape == 'f'  ? '\f'
                                 : escape == 'n'  ? '\n'
                                 : escape == 'r'  ? '\r'
                                 : escape == 't'  ? '\t'
                                                  : '\0';
            if (decoded == '\0' && escape != 'b') {
                set_error(PersistenceErrorCode::InvalidJson, position - 1);
                return false;
            }
            if (!append_codepoint(static_cast<unsigned char>(decoded), output, decoded_bytes,
                                  start))
                return false;
            continue;
        }
        if (byte < 0x80) {
            ++decoded_bytes;
            if (decoded_bytes > limits_.max_string_bytes) {
                set_error(PersistenceErrorCode::LimitExceeded, start, decoded_bytes,
                          limits_.max_string_bytes);
                return false;
            }
            output.push_back(static_cast<char>(byte));
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
            set_error(PersistenceErrorCode::InvalidUtf8, position - 1);
            return false;
        }
        if (source_.size() - position < continuation) {
            set_error(PersistenceErrorCode::InvalidUtf8, position);
            return false;
        }
        if (decoded_bytes + continuation + 1 > limits_.max_string_bytes) {
            set_error(PersistenceErrorCode::LimitExceeded, start,
                      decoded_bytes + continuation + 1,
                      limits_.max_string_bytes);
            return false;
        }
        decoded_bytes += continuation + 1;
        output.push_back(static_cast<char>(byte));
        for (std::size_t index = 0; index < continuation; ++index) {
            const auto next = static_cast<unsigned char>(source_[position++]);
            if ((next & 0xc0) != 0x80) {
                set_error(PersistenceErrorCode::InvalidUtf8, position - 1);
                return false;
            }
            output.push_back(static_cast<char>(next));
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
            set_error(PersistenceErrorCode::InvalidUtf8, position);
            return false;
        }
    }
    set_error(PersistenceErrorCode::InvalidJson, position);
    return false;
}

bool JsonSpanReader::decoded_string(JsonSpan span, std::string& value, std::string path) {
    if (span.begin >= span.end || source_[span.begin] != '"') {
        set_error(PersistenceErrorCode::UnexpectedType, span.begin, 0, 0, std::move(path));
        return false;
    }
    std::size_t position = span.begin;
    if (!decode_string(position, value) || position != span.end) {
        if (!has_error_)
            set_error(PersistenceErrorCode::InvalidJson, position, 0, 0, std::move(path));
        else if (error_.path.empty())
            error_.path = std::move(path);
        return false;
    }
    return true;
}

bool JsonSpanReader::canonical_u64(JsonSpan span, std::uint64_t& value, std::string path) {
    std::string text;
    if (!decoded_string(span, text, path))
        return false;
    if (text.empty()) {
        set_error(PersistenceErrorCode::UnexpectedType, span.begin, 0, 0, std::move(path));
        return false;
    }
    if (text.front() == '+' || (text.front() == '0' && text.size() != 1)) {
        set_error(PersistenceErrorCode::InvalidNumber, span.begin, 0, 0, std::move(path));
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        set_error(PersistenceErrorCode::InvalidNumber, span.begin, 0, 0, std::move(path));
        return false;
    }
    return true;
}

bool JsonSpanReader::u32_value(JsonSpan span, std::uint32_t& value) const noexcept {
    const auto raw = source_.substr(span.begin, span.end - span.begin);
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == raw.data() + raw.size();
}

} // namespace pulp::timeline::detail
