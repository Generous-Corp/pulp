// design_tweaks.cpp — the self-describing EDITMODE parameter block: locate,
// read, and surgically rewrite it while preserving every byte outside it.

#include <pulp/design/design_tweaks.hpp>

#include <choc/text/choc_JSON.h>
#include <choc/text/choc_UTF8.h>

#include <cctype>
#include <stdexcept>

namespace pulp::design {

namespace {

// choc's JSON writer iterates bytes with an unchecked UTF8Pointer (the bounds
// assert is compiled out in Release), so a truncated multibyte sequence would
// read past the buffer. Reject non-UTF-8 before handing bytes to the writer.
bool is_valid_utf8(std::string_view s) {
    return choc::text::findInvalidUTF8Data(s.data(), s.size()) == nullptr;
}

// Validate that `json_text` is exactly one JSON value (scalar or composite) and
// return its canonical serialization. We parse it wrapped as {"v":<text>}: this
// rejects malformed input, and because we keep only member "v" and re-serialize
// it, any bytes an attacker appended after the value are dropped (choc tolerates
// trailing content, so a raw single-parse check would be bypassable). A repeated
// "v" (e.g. text `1,"v":2`) makes choc throw on the duplicate key. choc's parser
// also validates UTF-8, so this covers non-UTF-8 values too. nullopt on failure.
std::optional<std::string> normalize_json_value(std::string_view json_text) {
    std::string wrapped = "{\"v\":";
    wrapped.append(json_text);
    wrapped += "}";
    choc::value::Value parsed;
    try {
        parsed = choc::json::parse(wrapped);
    } catch (...) {
        return std::nullopt;
    }
    if (!parsed.isObject() || !parsed.hasObjectMember("v")) return std::nullopt;
    return choc::json::toString(parsed["v"]);
}

// Scan one balanced JSON object starting at or after `start` (leading
// whitespace skipped), respecting string literals and backslash escapes so
// braces or marker bytes *inside* a string do not terminate it. Returns the
// offset just past the object's closing '}', or npos if there is no
// well-formed object there.
size_t scan_json_object(std::string_view s, size_t start) {
    size_t i = start;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size() || s[i] != '{') return std::string_view::npos;
    int depth = 0;
    bool in_string = false, escaped = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) return i + 1;
        }
    }
    return std::string_view::npos;  // unbalanced
}

}  // namespace

std::optional<EditBlockSpan> find_edit_block(std::string_view text) {
    // Scan every opening marker, not just the first: a literal "/*EDITMODE-BEGIN*/"
    // in a JS string or comment can precede the real block, so a malformed match
    // must not shadow a well-formed one later in the file.
    for (size_t search = 0;;) {
        size_t b = text.find(kEditBlockBegin, search);
        if (b == std::string_view::npos) return std::nullopt;
        size_t payload_begin = b + kEditBlockBegin.size();

        // The payload must be exactly one balanced JSON object; scan it
        // string-aware so a value containing the literal end-marker does not
        // truncate the block, and so trailing junk after the object (e.g.
        // `{...};evil()`) is rejected rather than silently dropped on a rewrite.
        size_t obj_end = scan_json_object(text, payload_begin);
        if (obj_end != std::string_view::npos) {
            // Only whitespace may sit between the object and the closing marker.
            size_t j = obj_end;
            while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
            if (text.substr(j, kEditBlockEnd.size()) == kEditBlockEnd) {
                EditBlockSpan span;
                span.block_begin = b;
                span.payload_begin = payload_begin;
                span.payload_end = obj_end;  // exactly the object (trailing ws stays outside)
                span.block_end = j + kEditBlockEnd.size();
                return span;
            }
        }
        search = b + 1;  // this marker did not start a valid block; try a later one
    }
}

std::optional<std::vector<TweakParam>> read_edit_block(std::string_view text) {
    auto span = find_edit_block(text);
    if (!span) return std::nullopt;
    std::string payload(text.substr(span->payload_begin, span->payload_end - span->payload_begin));

    // choc::json::parse returns an owning Value; bind it before touching members.
    choc::value::Value parsed;
    try {
        parsed = choc::json::parse(payload);
    } catch (const std::exception&) {
        return std::nullopt;  // malformed JSON payload
    }
    if (!parsed.isObject()) return std::nullopt;

    // A duplicate key would have thrown in parse() above (JSON objects may not
    // repeat a key), so member names here are already unique; preserve order.
    std::vector<TweakParam> params;
    params.reserve(parsed.size());
    for (uint32_t i = 0; i < parsed.size(); ++i) {
        auto member = parsed.getObjectMemberAt(i);
        params.push_back({std::string(member.name), choc::json::toString(member.value)});
    }
    return params;
}

std::optional<std::string> edit_block_payload(const std::vector<TweakParam>& params) {
    std::string out = "{";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) out += ',';
        // A key with invalid UTF-8 would over-read in choc's writer; reject it.
        if (!is_valid_utf8(params[i].key)) return std::nullopt;
        // Escape the key through choc so an odd key ("a\"b") stays valid JSON.
        out += choc::json::toString(choc::value::createString(params[i].key));
        out += ':';
        // Never emit a stored value verbatim: normalize it to exactly one JSON
        // value so a caller cannot smuggle a marker or extra structure into the
        // block (the CLI sanitizes, but this is the library trust boundary).
        auto value = normalize_json_value(params[i].json_value);
        if (!value) return std::nullopt;
        out += *value;
    }
    out += '}';
    return out;
}

std::optional<RewriteResult> rewrite_edit_block(std::string_view text,
                                                const std::vector<TweakParam>& params) {
    auto span = find_edit_block(text);
    if (!span) return std::nullopt;

    auto payload = edit_block_payload(params);
    if (!payload) return std::nullopt;  // a key/value failed validation
    RewriteResult r;
    r.text.reserve(text.size() + payload->size());
    r.text.append(text.substr(0, span->payload_begin));
    r.text.append(*payload);
    r.text.append(text.substr(span->payload_end));

    // Self-check that actually detects corruption: re-locate the block in the
    // rewritten text. It must start at the same offset, carry exactly the payload
    // we wrote, and leave every byte outside the whole marker span byte-identical
    // to the original. (The old check compared the new text's slices against the
    // very bytes just copied in, so it was always true and caught nothing.)
    auto check = find_edit_block(r.text);
    std::string_view out(r.text);
    r.outside_bytes_intact =
        check.has_value() && check->block_begin == span->block_begin &&
        out.substr(0, check->block_begin) == text.substr(0, span->block_begin) &&
        out.substr(check->block_end) == text.substr(span->block_end) &&
        out.substr(check->payload_begin, check->payload_end - check->payload_begin) == *payload;
    return r;
}

std::optional<RewriteResult> set_edit_param(std::string_view text, std::string_view key,
                                            std::string_view json_value) {
    auto params = read_edit_block(text);
    if (!params) return std::nullopt;
    bool found = false;
    for (auto& p : *params) {
        if (p.key == key) {
            p.json_value = std::string(json_value);
            found = true;
            break;
        }
    }
    if (!found) params->push_back({std::string(key), std::string(json_value)});
    return rewrite_edit_block(text, *params);
}

}  // namespace pulp::design
