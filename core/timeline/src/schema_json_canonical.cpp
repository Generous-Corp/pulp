#include <pulp/timeline/schema_json.hpp>

#include "schema_json_write_internal.hpp"

#include <algorithm>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::size_t offset = 0,
                                          std::uint64_t actual = 0, std::uint64_t limit = 0,
                                          std::string path = {}) {
    return runtime::Result<T, PersistenceError>(
        runtime::Err(PersistenceError{code, offset, actual, limit, std::move(path), std::nullopt}));
}

struct DecimalExponent {
    bool negative = false;
    std::string magnitude = "0";
};

std::string_view trim_decimal_zeroes(std::string_view value) noexcept {
    while (value.size() > 1 && value.front() == '0')
        value.remove_prefix(1);
    return value;
}

int compare_decimal_magnitudes(std::string_view lhs, std::string_view rhs) noexcept {
    lhs = trim_decimal_zeroes(lhs);
    rhs = trim_decimal_zeroes(rhs);
    if (lhs.size() != rhs.size())
        return lhs.size() < rhs.size() ? -1 : 1;
    const auto order = lhs.compare(rhs);
    return order < 0 ? -1 : order > 0 ? 1 : 0;
}

std::string add_decimal_magnitudes(std::string_view lhs, std::string_view rhs) {
    std::string reversed;
    reversed.reserve(std::max(lhs.size(), rhs.size()) + 1);
    std::size_t left = lhs.size();
    std::size_t right = rhs.size();
    unsigned carry = 0;
    while (left != 0 || right != 0 || carry != 0) {
        unsigned digit = carry;
        if (left != 0)
            digit += static_cast<unsigned>(lhs[--left] - '0');
        if (right != 0)
            digit += static_cast<unsigned>(rhs[--right] - '0');
        reversed.push_back(static_cast<char>('0' + digit % 10));
        carry = digit / 10;
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

std::string subtract_decimal_magnitudes(std::string_view larger, std::string_view smaller) {
    std::string reversed;
    reversed.reserve(larger.size());
    std::size_t left = larger.size();
    std::size_t right = smaller.size();
    int borrow = 0;
    while (left != 0) {
        int digit = static_cast<int>(larger[--left] - '0') - borrow;
        if (right != 0)
            digit -= static_cast<int>(smaller[--right] - '0');
        if (digit < 0) {
            digit += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        reversed.push_back(static_cast<char>('0' + digit));
    }
    while (reversed.size() > 1 && reversed.back() == '0')
        reversed.pop_back();
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

void add_to_exponent(DecimalExponent& exponent, bool negative, std::string_view magnitude) {
    magnitude = trim_decimal_zeroes(magnitude);
    if (magnitude == "0")
        return;
    if (exponent.magnitude == "0") {
        exponent.negative = negative;
        exponent.magnitude = magnitude;
        return;
    }
    if (exponent.negative == negative) {
        exponent.magnitude = add_decimal_magnitudes(exponent.magnitude, magnitude);
        return;
    }
    const auto order = compare_decimal_magnitudes(exponent.magnitude, magnitude);
    if (order == 0) {
        exponent = {};
    } else if (order > 0) {
        exponent.magnitude = subtract_decimal_magnitudes(exponent.magnitude, magnitude);
    } else {
        exponent.negative = negative;
        exponent.magnitude = subtract_decimal_magnitudes(magnitude, exponent.magnitude);
    }
}

void add_size_to_exponent(DecimalExponent& exponent, bool negative, std::size_t value) {
    add_to_exponent(exponent, negative, std::to_string(value));
}

DecimalExponent parse_decimal_exponent(std::string_view value) {
    DecimalExponent result;
    if (value.empty())
        return result;
    result.negative = value.front() == '-';
    if (result.negative || value.front() == '+')
        value.remove_prefix(1);
    result.magnitude = trim_decimal_zeroes(value);
    if (result.magnitude == "0")
        result.negative = false;
    return result;
}

std::optional<int> small_decimal_exponent(const DecimalExponent& exponent) noexcept {
    if (exponent.magnitude.size() > 2)
        return std::nullopt;
    int value = 0;
    for (const auto digit : exponent.magnitude)
        value = value * 10 + digit - '0';
    return exponent.negative ? -value : value;
}

std::string canonical_number(std::string_view source) {
    const bool negative = source.starts_with('-');
    if (negative)
        source.remove_prefix(1);
    const auto exponent_at = source.find_first_of("eE");
    const auto significand = source.substr(0, exponent_at);
    const auto exponent_source =
        exponent_at == std::string_view::npos ? std::string_view{} : source.substr(exponent_at + 1);
    const auto decimal_at = significand.find('.');
    const auto integer = significand.substr(0, decimal_at);
    const auto fraction = decimal_at == std::string_view::npos ? std::string_view{}
                                                               : significand.substr(decimal_at + 1);

    std::string digits;
    digits.reserve(integer.size() + fraction.size());
    digits.append(integer);
    digits.append(fraction);
    const auto first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos)
        return "0";
    digits.erase(0, first_nonzero);
    std::size_t trailing_zeroes = 0;
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
        ++trailing_zeroes;
    }

    auto exponent = parse_decimal_exponent(exponent_source);
    add_size_to_exponent(exponent, true, fraction.size());
    add_size_to_exponent(exponent, false, trailing_zeroes);
    add_size_to_exponent(exponent, false, digits.size() - 1);

    std::string output;
    if (negative)
        output.push_back('-');
    const auto compact = small_decimal_exponent(exponent);
    if (compact && *compact >= -6 && *compact < 21) {
        const int decimal_position = *compact + 1;
        if (decimal_position <= 0) {
            output += "0.";
            output.append(static_cast<std::size_t>(-decimal_position), '0');
            output += digits;
        } else if (static_cast<std::size_t>(decimal_position) >= digits.size()) {
            output += digits;
            output.append(static_cast<std::size_t>(decimal_position) - digits.size(), '0');
        } else {
            output.append(digits, 0, static_cast<std::size_t>(decimal_position));
            output.push_back('.');
            output.append(digits, static_cast<std::size_t>(decimal_position));
        }
        return output;
    }

    output.push_back(digits.front());
    if (digits.size() > 1) {
        output.push_back('.');
        output.append(digits, 1);
    }
    output.push_back('e');
    if (exponent.negative)
        output.push_back('-');
    output += exponent.magnitude;
    return output;
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
    case JsonValue::Kind::Number:
        // The parser has already validated the JSON-number grammar. Normalize
        // its decimal value exactly, without floating-point rounding or an
        // exponent-magnitude limit.
        return runtime::Result<std::string, PersistenceError>(
            runtime::Ok(canonical_number(value.scalar)));
    case JsonValue::Kind::Array: {
        std::string output = "[";
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            auto element = canonical(value.array[index]);
            if (!element)
                return element;
            if (index != 0)
                output.push_back(',');
            output += std::move(element).value();
        }
        output.push_back(']');
        return runtime::Result<std::string, PersistenceError>(runtime::Ok(std::move(output)));
    }
    case JsonValue::Kind::Object: {
        std::vector<const std::pair<std::string, JsonValue>*> members;
        members.reserve(value.object.size());
        for (const auto& member : value.object)
            members.push_back(&member);
        std::sort(members.begin(), members.end(),
                  [](const auto* lhs, const auto* rhs) { return lhs->first < rhs->first; });
        std::string output = "{";
        for (std::size_t index = 0; index < members.size(); ++index) {
            auto encoded = canonical(members[index]->second);
            if (!encoded)
                return encoded;
            if (index != 0)
                output.push_back(',');
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
    detail::append_quoted_json_string(value, [&output](std::string_view text) {
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
