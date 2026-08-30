/// @file canonical_json.cpp
/// The canonical JSON form, and the only place that decides it.
///
/// ─────────────────────────────────────────────────────────────────────────
/// THE CANONICAL FORM
/// ─────────────────────────────────────────────────────────────────────────
///
/// Every capsule digest is a SHA-256 over bytes produced by `write_value`
/// below, so this list is not a description of the code: it is the format.
/// Changing any line of it changes `revision_id` for every capsule ever
/// exported, which is a format version bump, not a refactor.
///
///  1. Encoding — UTF-8, no byte-order mark. A string that is not well-formed
///     UTF-8 is rejected rather than repaired: overlong encodings, the UTF-16
///     surrogate block (U+D800..U+DFFF, which has no UTF-8 encoding), and
///     anything past U+10FFFF all fail. Repairing would let two different
///     inputs canonicalize to the same bytes.
///
///  2. Whitespace — none. No space after `:` or `,`, no indentation, no
///     trailing newline. The output therefore contains no CR and no LF
///     outside of string escapes, which is what makes the "LF line endings"
///     rule trivially true: there is nothing for a CRLF filesystem to
///     rewrite, so a capsule written on Windows digests like one written on
///     macOS.
///
///  3. Object keys — sorted ascending by the raw UTF-8 bytes of the key, as
///     unsigned octets. UTF-8 byte order and Unicode code point order agree,
///     so this is code point order without decoding anything. Two members
///     with the same name have no canonical order and are rejected.
///
///  4. Strings — `"` and `\` are escaped; U+0000..U+001F use the short forms
///     `\b` `\t` `\n` `\f` `\r` where one exists and lowercase `\u00xx`
///     otherwise. Nothing else is escaped: `/` is emitted bare and every
///     character above U+001F is emitted as its raw UTF-8 bytes. Escaping is
///     minimal so the encoder has exactly one choice per character.
///
///  5. Numbers — the shortest decimal that reads back as the identical
///     double, laid out by the ECMAScript `Number::toString` rule (the same
///     rule RFC 8785 adopts):
///
///       let the value be `digits x 10^(n - k)`, where `digits` carries no
///       leading or trailing zero and `k` is its length;
///         k <= n <= 21   ->  digits followed by (n - k) zeros
///         0 < n <= 21    ->  digits with a '.' after the first n
///         -6 < n <= 0    ->  "0." then (-n) zeros then digits
///         otherwise      ->  d[0] ["." d[1..]] "e" ('+'|'-') abs(n - 1)
///
///     So an exact integer never grows a decimal point or an exponent (`1.0`
///     and `1` both emit `1`), `-0` emits `0`, and no digit is printed that
///     the value does not require. NaN and both infinities have no JSON
///     spelling and are rejected.
///
///     Only the DIGITS come from the toolchain (`std::to_chars`, whose
///     shortest-round-trip result is mathematically pinned by the standard);
///     the layout above is reassembled here, so the output cannot drift with
///     a library's choice of style, exponent width, or zero padding.
///
///  6. Arrays — element order is preserved. It is data, not presentation.
///     The one ordering the format imposes on itself is `files[]` sorted by
///     path in byte order, applied by the envelope before it serializes.
///
///  7. Literals — `true`, `false`, `null`, lowercase.
///
/// Parsing is choc's; serializing is ours. choc's writer emits members in
/// insertion order and formats numbers through `std::ostringstream`, so it
/// can round-trip a document but cannot canonicalize one.

#include "canonical_json.hpp"

#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <system_error>
#include <utility>

namespace pulp::authoring_capsule::detail {
namespace {

using runtime::Err;
using runtime::Ok;

CapsuleError invalid(std::string_view subject, std::string_view required, std::string_view found) {
    CapsuleError error;
    error.status = CapsuleStatus::manifest_invalid;
    error.subject = std::string(subject);
    error.required = std::string(required);
    error.found = std::string(found);
    return error;
}

template <typename T>
runtime::Result<T, CapsuleError> fail(std::string_view subject, std::string_view required,
                                      std::string_view found) {
    return runtime::Result<T, CapsuleError>(Err(invalid(subject, required, found)));
}

/// Append one RFC 6901 reference token, escaping `~` and `/` so a key that
/// contains a slash still yields an unambiguous pointer.
std::string child_pointer(std::string_view parent, std::string_view token) {
    std::string out(parent);
    out += '/';
    for (const char c : token) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

/// What a value is, for the `found` half of a rejection. A person fixing a
/// manifest needs to know the member was a string where a number belonged.
std::string_view type_name(const JsonView& value) {
    if (value.isVoid()) return "null";
    if (value.isBool()) return "bool";
    if (value.isInt() || value.isFloat()) return "number";
    if (value.isString()) return "string";
    if (value.isArray() || value.isVector()) return "array";
    if (value.isObject()) return "object";
    return "an unrepresentable value";
}

// ── Numbers ──────────────────────────────────────────────────────────────

/// Shortest round-tripping digits for a finite, non-zero double, in
/// scientific style. The caller reassembles the layout; this only sources the
/// digits.
std::string shortest_scientific(double value) {
    char buffer[64];
#if defined(PULP_CAPSULE_NO_FLOAT_TO_CHARS)
    // Escape hatch for a toolchain without floating-point `std::to_chars`:
    // ask printf for successively more digits and stop at the first precision
    // that reads back bit-identical. 17 significant digits always round-trip a
    // double, so the loop terminates.
    for (int precision = 0; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof buffer, "%.*e", precision, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    return std::string(buffer);
#else
    // `__cpp_lib_to_chars` is the WRONG gate here and must not be used:
    // libc++ withholds that macro because floating-point `from_chars` is
    // missing, while `to_chars(double)` has shipped since LLVM 14 and is
    // available at Pulp's macOS deployment floor (13.4 > the 13.3 the Apple
    // SDK annotates it with). GCC 11+ and MSVC 19.14+ have it too. A
    // toolchain that genuinely lacks it defines PULP_CAPSULE_NO_FLOAT_TO_CHARS
    // and takes the branch above.
    const auto result =
        std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
    if (result.ec != std::errc{}) return {};
    return std::string(buffer, static_cast<std::size_t>(result.ptr - buffer));
#endif
}

/// A decimal decomposed as `digits x 10^(n - k)` with no leading or trailing
/// zero in `digits`, which is the shape the ECMAScript layout rule is written
/// against.
struct Decimal {
    bool negative = false;
    std::string digits;
    int n = 0;
};

bool decompose(std::string_view text, Decimal& out) {
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
        out.negative = text[i] == '-';
        ++i;
    }

    std::string mantissa;
    int point = -1;
    for (; i < text.size(); ++i) {
        if (text[i] >= '0' && text[i] <= '9') {
            mantissa.push_back(text[i]);
            continue;
        }
        if (text[i] == '.' && point < 0) {
            point = static_cast<int>(mantissa.size());
            continue;
        }
        break;
    }
    if (mantissa.empty()) return false;
    if (point < 0) point = static_cast<int>(mantissa.size());

    int exponent = 0;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool negative_exponent = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            negative_exponent = text[i] == '-';
            ++i;
        }
        if (i >= text.size()) return false;
        for (; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            exponent = exponent * 10 + (text[i] - '0');
            // A double's decimal exponent never leaves [-324, 309]; anything
            // past this is malformed input, and the bound keeps the running
            // accumulate from overflowing.
            if (exponent > 100000) return false;
        }
        if (negative_exponent) exponent = -exponent;
    }
    if (i != text.size()) return false;

    // value == mantissa x 10^(exponent - digits_after_point)
    int scale = exponent - (static_cast<int>(mantissa.size()) - point);

    std::size_t first = 0;
    while (first + 1 < mantissa.size() && mantissa[first] == '0') ++first;
    std::size_t last = mantissa.size();
    while (last > first + 1 && mantissa[last - 1] == '0') {
        --last;
        ++scale;  // dropping a trailing zero divides the integer by ten
    }

    out.digits.assign(mantissa, first, last - first);
    if (out.digits == "0") {
        out.n = 1;  // caller handles zero before reaching here; stay defined
        return true;
    }
    out.n = scale + static_cast<int>(out.digits.size());
    return true;
}

void append_exponent(std::string& out, int exponent) {
    out += 'e';
    out += exponent < 0 ? '-' : '+';
    char buffer[16];
    const auto magnitude = exponent < 0 ? -static_cast<long long>(exponent) : exponent;
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, magnitude);
    out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

std::string layout(const Decimal& decimal) {
    const auto& digits = decimal.digits;
    const int k = static_cast<int>(digits.size());
    const int n = decimal.n;

    std::string out;
    if (decimal.negative) out += '-';

    if (k <= n && n <= 21) {
        out += digits;
        out.append(static_cast<std::size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out.append(digits, 0, static_cast<std::size_t>(n));
        out += '.';
        out.append(digits, static_cast<std::size_t>(n), std::string::npos);
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-n), '0');
        out += digits;
    } else if (k == 1) {
        out += digits;
        append_exponent(out, n - 1);
    } else {
        out += digits[0];
        out += '.';
        out.append(digits, 1, std::string::npos);
        append_exponent(out, n - 1);
    }
    return out;
}

// ── Strings ──────────────────────────────────────────────────────────────

constexpr bool is_continuation(unsigned char byte) {
    return (byte & 0xC0u) == 0x80u;
}

std::optional<CapsuleError> append_quoted(std::string& out, std::string_view text,
                                          std::string_view pointer) {
    static constexpr char kHex[] = "0123456789abcdef";

    out += '"';
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80u) {
            switch (byte) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (byte < 0x20u) {
                        out += "\\u00";
                        out += kHex[byte >> 4];
                        out += kHex[byte & 0x0Fu];
                    } else {
                        out += static_cast<char>(byte);
                    }
                    break;
            }
            ++i;
            continue;
        }

        // The lead-byte ranges and the narrowed second-byte range together
        // reject overlong encodings, the surrogate block, and anything past
        // U+10FFFF, so whatever is copied through is well-formed UTF-8.
        std::size_t length = 0;
        unsigned char second_low = 0x80u;
        unsigned char second_high = 0xBFu;
        if (byte >= 0xC2u && byte <= 0xDFu) {
            length = 2;
        } else if (byte == 0xE0u) {
            length = 3;
            second_low = 0xA0u;
        } else if (byte >= 0xE1u && byte <= 0xECu) {
            length = 3;
        } else if (byte == 0xEDu) {
            length = 3;
            second_high = 0x9Fu;
        } else if (byte >= 0xEEu && byte <= 0xEFu) {
            length = 3;
        } else if (byte == 0xF0u) {
            length = 4;
            second_low = 0x90u;
        } else if (byte >= 0xF1u && byte <= 0xF3u) {
            length = 4;
        } else if (byte == 0xF4u) {
            length = 4;
            second_high = 0x8Fu;
        } else {
            return invalid(pointer, "well-formed UTF-8", "an invalid lead byte");
        }

        if (i + length > text.size()) {
            return invalid(pointer, "well-formed UTF-8", "a truncated sequence");
        }
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if (second < second_low || second > second_high) {
            return invalid(pointer, "well-formed UTF-8", "an invalid continuation byte");
        }
        for (std::size_t k = 2; k < length; ++k) {
            if (!is_continuation(static_cast<unsigned char>(text[i + k]))) {
                return invalid(pointer, "well-formed UTF-8", "an invalid continuation byte");
            }
        }
        out.append(text.substr(i, length));
        i += length;
    }
    out += '"';
    return std::nullopt;
}

// ── Serializing ──────────────────────────────────────────────────────────
//
// These recurse and are checked at every level, so they return
// `std::optional<CapsuleError>` — engaged means rejected — which reads as
// `if (auto problem = ...) return problem;` without a Result wrapper at each
// hop. The unit's own surface still speaks `runtime::Result`.

std::optional<CapsuleError> write_value(std::string& out, const JsonView& value,
                                        const std::string& pointer, std::size_t depth);

std::optional<CapsuleError> write_object(std::string& out, const JsonView& value,
                                         const std::string& pointer, std::size_t depth) {
    const auto count = value.size();
    std::vector<std::pair<std::string_view, JsonView>> members;
    members.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto member = value.getObjectMemberAt(i);
        members.emplace_back(std::string_view(member.name), member.value);
    }

    // `std::string_view::operator<` compares through char_traits<char>, whose
    // ordering is defined on unsigned char — raw UTF-8 byte order, which is
    // also Unicode code point order.
    std::stable_sort(members.begin(), members.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    out += '{';
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (i != 0) {
            if (members[i].first == members[i - 1].first) {
                return invalid(child_pointer(pointer, members[i].first), "one member per name",
                               "a duplicate name");
            }
            out += ',';
        }
        if (auto problem = append_quoted(out, members[i].first, pointer)) return problem;
        out += ':';
        const auto child = child_pointer(pointer, members[i].first);
        if (auto problem = write_value(out, members[i].second, child, depth + 1)) return problem;
    }
    out += '}';
    return std::nullopt;
}

std::optional<CapsuleError> write_array(std::string& out, const JsonView& value,
                                        const std::string& pointer, std::size_t depth) {
    out += '[';
    const auto count = value.size();
    for (std::uint32_t i = 0; i < count; ++i) {
        if (i != 0) out += ',';
        const auto child = child_pointer(pointer, canonical_number(static_cast<std::int64_t>(i)));
        if (auto problem = write_value(out, value[i], child, depth + 1)) return problem;
    }
    out += ']';
    return std::nullopt;
}

std::optional<CapsuleError> write_value(std::string& out, const JsonView& value,
                                        const std::string& pointer, std::size_t depth) {
    if (depth > kMaxJsonDepth) {
        return invalid(pointer, "nesting within the depth bound", "deeper nesting");
    }
    if (value.isVoid()) {
        out += "null";
        return std::nullopt;
    }
    if (value.isBool()) {
        out += value.getBool() ? "true" : "false";
        return std::nullopt;
    }
    if (value.isInt32()) {
        out += canonical_number(static_cast<std::int64_t>(value.getInt32()));
        return std::nullopt;
    }
    if (value.isInt64()) {
        out += canonical_number(value.getInt64());
        return std::nullopt;
    }
    if (value.isFloat()) {
        // JSON has one numeric type, so a float32 is widened rather than
        // printed at float precision: the value the document carries is the
        // double, and printing `0.1` for a float32 would not read back as the
        // same bits.
        const double number =
            value.isFloat32() ? static_cast<double>(value.getFloat32()) : value.getFloat64();
        auto text = canonical_number(number, pointer);
        if (text.is_err()) return std::move(text).error();
        out += text.value();
        return std::nullopt;
    }
    if (value.isString()) {
        return append_quoted(out, value.getString(), pointer);
    }
    if (value.isArray() || value.isVector()) {
        return write_array(out, value, pointer, depth);
    }
    if (value.isObject()) {
        return write_object(out, value, pointer, depth);
    }
    return invalid(pointer, "a JSON value", type_name(value));
}

// ── Pre-parse admission ──────────────────────────────────────────────────

/// Structural checks choc's parser does not make, run before it sees the
/// text.
///
/// Two of these are not stylistic: choc's `parseTopLevel` returns as soon as
/// the outermost value closes, so `{"a":1} anything` parses clean, and its
/// number scanner hands the buffer to `strtoll`/`strtod`, which read to a NUL
/// terminator. A capsule is bytes from another machine, so both are checked
/// here rather than trusted.
std::optional<CapsuleError> prescan(std::string_view text, std::string_view subject) {
    std::vector<char> open;
    open.reserve(kMaxJsonDepth);

    bool in_string = false;
    bool escaped = false;
    bool closed = false;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\0') {
            return invalid(subject, "text without NUL bytes", "an embedded NUL");
        }
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (closed) {
            // Everything after the outermost value must be whitespace: a
            // second document appended to the first would otherwise be
            // silently ignored, and the bytes a signature covers would not be
            // the bytes that were parsed.
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            return invalid(subject, "one JSON value", "trailing content");
        }
        switch (c) {
            case '"': in_string = true; break;
            case '{':
            case '[':
                if (open.size() >= kMaxJsonDepth) {
                    return invalid(subject, "nesting within the depth bound", "deeper nesting");
                }
                open.push_back(c);
                break;
            case '}':
            case ']': {
                if (open.empty()) return invalid(subject, "balanced brackets", "an unopened close");
                const char expected = open.back() == '{' ? '}' : ']';
                if (c != expected) {
                    return invalid(subject, "balanced brackets", "a mismatched close");
                }
                open.pop_back();
                if (open.empty()) closed = true;
                break;
            }
            default: break;
        }
    }

    if (in_string) return invalid(subject, "a terminated string", "an unterminated string");
    if (!open.empty()) return invalid(subject, "balanced brackets", "an unclosed bracket");
    if (!closed) return invalid(subject, "a JSON object or array", "no value");
    return std::nullopt;
}

runtime::Result<Json, CapsuleError> parse_error(std::string_view subject, const std::string& what) {
    return fail<Json>(subject, "parseable JSON", what);
}

}  // namespace

// ── Numbers ──────────────────────────────────────────────────────────────

std::string canonical_number(std::int64_t value) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

runtime::Result<std::string, CapsuleError> canonical_number(double value,
                                                            std::string_view subject) {
    using R = runtime::Result<std::string, CapsuleError>;
    if (std::isnan(value)) return fail<std::string>(subject, "a finite number", "NaN");
    if (std::isinf(value)) return fail<std::string>(subject, "a finite number", "infinity");
    // Negative zero collapses to `0`, matching the ECMAScript rule. Two
    // exports that differ only in the sign of a zero are the same revision.
    if (value == 0.0) return R(Ok(std::string("0")));

    const auto scientific = shortest_scientific(value);
    Decimal decimal;
    if (scientific.empty() || !decompose(scientific, decimal)) {
        return fail<std::string>(subject, "a finite number", scientific);
    }
    return R(Ok(layout(decimal)));
}

// ── Parsing ──────────────────────────────────────────────────────────────

runtime::Result<Json, CapsuleError> parse_json(std::string_view text, std::string_view subject) {
    using R = runtime::Result<Json, CapsuleError>;

    if (auto problem = prescan(text, subject)) return R(Err(std::move(*problem)));

    // choc walks the buffer as a C string and hands the same pointer to
    // strtoll/strtod, so it reads past the end of a view that is not itself
    // NUL-terminated. A manifest arrives as a span over archive bytes, which
    // is exactly that case, so copy into a std::string first.
    const std::string buffer(text);

    try {
        auto parsed = choc::json::parse(std::string_view(buffer));
        if (parsed.isVoid()) {
            return fail<Json>(subject, "a JSON object or array", "nothing");
        }
        return R(Ok(std::move(parsed)));
    } catch (const choc::json::ParseError& error) {
        std::string where(error.what());
        if (error.lineAndColumn.isValid()) {
            where += " at line ";
            where += canonical_number(static_cast<std::int64_t>(error.lineAndColumn.line));
            where += ", column ";
            where += canonical_number(static_cast<std::int64_t>(error.lineAndColumn.column));
        }
        return parse_error(subject, where);
    } catch (const choc::value::Error& error) {
        return parse_error(subject, error.what());
    } catch (const std::exception& error) {
        return parse_error(subject, error.what());
    }
}

runtime::Result<Json, CapsuleError> parse_json_object(std::string_view text,
                                                      std::string_view subject) {
    using R = runtime::Result<Json, CapsuleError>;
    auto parsed = parse_json(text, subject);
    if (parsed.is_err()) return parsed;
    if (!parsed.value().isObject()) {
        return fail<Json>(subject, "object", type_name(parsed.value().getView()));
    }
    return R(Ok(std::move(parsed).value()));
}

// ── Serializing ──────────────────────────────────────────────────────────

runtime::Result<std::string, CapsuleError> to_canonical_text(const JsonView& value,
                                                             std::string_view pointer_root) {
    using R = runtime::Result<std::string, CapsuleError>;
    std::string out;
    const std::string pointer(pointer_root);
    if (auto problem = write_value(out, value, pointer, 0)) {
        return R(Err(std::move(*problem)));
    }
    return R(Ok(std::move(out)));
}

runtime::Result<std::string, CapsuleError> canonicalize_json_text(std::string_view text,
                                                                  std::string_view subject) {
    auto parsed = parse_json(text, subject);
    if (parsed.is_err()) {
        return runtime::Result<std::string, CapsuleError>(Err(std::move(parsed).error()));
    }
    return to_canonical_text(parsed.value().getView(), subject);
}

// ── Building ─────────────────────────────────────────────────────────────

Json json_object() {
    return choc::value::createObject({});
}

Json json_array() {
    return choc::value::createEmptyArray();
}

Json json_string_array(const std::vector<std::string>& values) {
    auto array = json_array();
    for (const auto& value : values) array.addArrayElement(std::string_view(value));
    return array;
}

void put(Json& object, std::string_view key, const JsonView& value) {
    object.setMember(key, value);
}

void put_string(Json& object, std::string_view key, std::string_view value) {
    object.setMember(key, value);
}

void put_int(Json& object, std::string_view key, std::int64_t value) {
    object.setMember(key, value);
}

void put_uint(Json& object, std::string_view key, std::uint64_t value) {
    // Byte counts and versions are the only unsigned fields in the envelope,
    // and every one of them is capped by the archive budgets far below
    // int64's range, so the narrowing cannot lose a representable value.
    object.setMember(key, static_cast<std::int64_t>(value));
}

void put_bool(Json& object, std::string_view key, bool value) {
    object.setMember(key, value);
}

runtime::Result<void, CapsuleError> put_json_text(Json& object, std::string_view key,
                                                  std::string_view json_text,
                                                  std::string_view subject) {
    const auto pointer = child_pointer(subject, key);
    auto parsed = parse_json(json_text, pointer);
    if (parsed.is_err()) return Err(std::move(parsed).error());
    object.setMember(key, parsed.value().getView());
    return {};
}

void append(Json& array, const JsonView& value) {
    array.addArrayElement(value);
}

// ── Unknown-key preservation ─────────────────────────────────────────────

Json unknown_members(const JsonView& source, std::span<const std::string_view> known) {
    auto out = json_object();
    if (!source.isObject()) return out;

    const auto count = source.size();
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto member = source.getObjectMemberAt(i);
        const std::string_view name(member.name);
        if (std::find(known.begin(), known.end(), name) != known.end()) continue;
        out.setMember(name, member.value);
    }
    return out;
}

void merge_absent_members(Json& object, const JsonView& extra) {
    if (!object.isObject() || !extra.isObject()) return;

    const auto count = extra.size();
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto member = extra.getObjectMemberAt(i);
        const std::string_view name(member.name);
        if (object.hasObjectMember(name)) continue;
        object.setMember(name, member.value);
    }
}

// ── Reading ──────────────────────────────────────────────────────────────

bool has_member(const JsonView& object, std::string_view key) {
    return object.isObject() && object.hasObjectMember(key);
}

runtime::Result<std::string, CapsuleError> require_string(const JsonView& object,
                                                          std::string_view key,
                                                          std::string_view pointer_root) {
    using R = runtime::Result<std::string, CapsuleError>;
    const auto pointer = child_pointer(pointer_root, key);
    if (!has_member(object, key)) return fail<std::string>(pointer, "string", "nothing");

    const auto member = object[key];
    if (!member.isString()) return fail<std::string>(pointer, "string", type_name(member));
    return R(Ok(std::string(member.getString())));
}

runtime::Result<std::uint64_t, CapsuleError> require_uint(const JsonView& object,
                                                          std::string_view key,
                                                          std::string_view pointer_root) {
    const auto pointer = child_pointer(pointer_root, key);
    if (!has_member(object, key)) {
        return fail<std::uint64_t>(pointer, "a non-negative integer", "nothing");
    }
    return optional_uint(object, key, 0, pointer_root);
}

runtime::Result<std::string, CapsuleError> optional_string(const JsonView& object,
                                                           std::string_view key,
                                                           std::string_view fallback,
                                                           std::string_view pointer_root) {
    using R = runtime::Result<std::string, CapsuleError>;
    if (!has_member(object, key)) return R(Ok(std::string(fallback)));
    return require_string(object, key, pointer_root);
}

runtime::Result<std::uint64_t, CapsuleError> optional_uint(const JsonView& object,
                                                           std::string_view key,
                                                           std::uint64_t fallback,
                                                           std::string_view pointer_root) {
    using R = runtime::Result<std::uint64_t, CapsuleError>;
    if (!has_member(object, key)) return R(Ok(fallback));

    const auto pointer = child_pointer(pointer_root, key);
    const auto member = object[key];

    if (member.isInt32() || member.isInt64()) {
        const std::int64_t value =
            member.isInt32() ? static_cast<std::int64_t>(member.getInt32()) : member.getInt64();
        if (value < 0) {
            return fail<std::uint64_t>(pointer, "a non-negative integer", canonical_number(value));
        }
        return R(Ok(static_cast<std::uint64_t>(value)));
    }

    if (member.isFloat()) {
        // A writer whose numbers are all floats (Python, some JS tooling)
        // spells a byte count `1024.0`. That is the same integer, so it is
        // accepted — but only while the double still represents every integer
        // exactly, so a rounded count can never be read back as exact.
        const double value =
            member.isFloat32() ? static_cast<double>(member.getFloat32()) : member.getFloat64();
        constexpr double kExactIntegerLimit = 9007199254740992.0;  // 2^53
        if (std::isfinite(value) && value >= 0.0 && value <= kExactIntegerLimit &&
            value == std::trunc(value)) {
            return R(Ok(static_cast<std::uint64_t>(value)));
        }
        auto found = canonical_number(value, pointer);
        return fail<std::uint64_t>(pointer, "a non-negative integer",
                                   found.has_value() ? found.value() : std::string("a number"));
    }

    return fail<std::uint64_t>(pointer, "a non-negative integer", type_name(member));
}

runtime::Result<bool, CapsuleError> optional_bool(const JsonView& object, std::string_view key,
                                                  bool fallback, std::string_view pointer_root) {
    using R = runtime::Result<bool, CapsuleError>;
    if (!has_member(object, key)) return R(Ok(fallback));

    const auto member = object[key];
    if (!member.isBool()) {
        return fail<bool>(child_pointer(pointer_root, key), "bool", type_name(member));
    }
    return R(Ok(member.getBool()));
}

runtime::Result<std::vector<std::string>, CapsuleError>
optional_string_array(const JsonView& object, std::string_view key, std::string_view pointer_root) {
    using R = runtime::Result<std::vector<std::string>, CapsuleError>;
    std::vector<std::string> values;
    if (!has_member(object, key)) return R(Ok(std::move(values)));

    const auto pointer = child_pointer(pointer_root, key);
    const auto member = object[key];
    if (!member.isArray()) {
        return fail<std::vector<std::string>>(pointer, "array", type_name(member));
    }

    const auto count = member.size();
    values.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto element = member[i];
        if (!element.isString()) {
            return fail<std::vector<std::string>>(
                child_pointer(pointer, canonical_number(static_cast<std::int64_t>(i))), "string",
                type_name(element));
        }
        values.emplace_back(element.getString());
    }
    return R(Ok(std::move(values)));
}

runtime::Result<std::string, CapsuleError> optional_json_text(const JsonView& object,
                                                              std::string_view key,
                                                              std::string_view fallback,
                                                              std::string_view pointer_root) {
    using R = runtime::Result<std::string, CapsuleError>;
    if (!has_member(object, key)) return R(Ok(std::string(fallback)));

    const auto pointer = child_pointer(pointer_root, key);
    const auto member = object[key];
    if (!member.isObject() && !member.isArray()) {
        return fail<std::string>(pointer, "an object or array", type_name(member));
    }
    return to_canonical_text(member, pointer);
}

// ── Subtree and digest ───────────────────────────────────────────────────

Json subtree(const JsonView& object, std::span<const std::string_view> keys) {
    auto out = json_object();
    if (!object.isObject()) return out;
    for (const auto key : keys) {
        if (object.hasObjectMember(key)) out.setMember(key, object[key]);
    }
    return out;
}

runtime::Result<void, CapsuleError> sort_array_by_string_member(Json& array,
                                                                std::string_view key,
                                                                std::string_view pointer_root) {
    if (!array.isArray()) {
        return Err(invalid(pointer_root, "array", type_name(array.getView())));
    }

    const auto count = array.size();
    std::vector<std::pair<std::string, std::uint32_t>> order;
    order.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto element = array[i];
        const auto pointer =
            child_pointer(pointer_root, canonical_number(static_cast<std::int64_t>(i)));
        if (!element.isObject()) {
            return Err(invalid(pointer, "object", type_name(element)));
        }
        if (!element.hasObjectMember(key)) {
            return Err(invalid(child_pointer(pointer, key), "string", "nothing"));
        }
        const auto member = element[key];
        if (!member.isString()) {
            return Err(invalid(child_pointer(pointer, key), "string", type_name(member)));
        }
        order.emplace_back(std::string(member.getString()), i);
    }

    // std::string's ordering is char_traits<char>'s, which is defined on
    // unsigned char: the byte order the format asks for.
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    auto sorted = json_array();
    for (const auto& entry : order) sorted.addArrayElement(array[entry.second]);
    array = std::move(sorted);
    return {};
}

runtime::Result<std::string, CapsuleError> canonical_sha256_hex(const JsonView& value) {
    using R = runtime::Result<std::string, CapsuleError>;
    auto text = to_canonical_text(value);
    if (text.is_err()) return text;
    return R(Ok(runtime::sha256_hex(std::string_view(text.value()))));
}

runtime::Result<std::string, CapsuleError> canonical_sha256_uri(const JsonView& value) {
    using R = runtime::Result<std::string, CapsuleError>;
    auto hex = canonical_sha256_hex(value);
    if (hex.is_err()) return hex;
    return R(Ok("sha256:" + std::move(hex).value()));
}

}  // namespace pulp::authoring_capsule::detail
