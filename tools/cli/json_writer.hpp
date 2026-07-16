// SPDX-License-Identifier: MIT
// Minimal JSON string writer for CLI report output. Escapes a string to a
// JSON-safe body (json_escape) or a full quoted JSON string (json_string).
// Header-only so standalone test targets link without an extra source file.
#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace pulp::cli {

// Escape a string for embedding inside a JSON string literal. Emits the
// short escapes for the standard control characters and \uXXXX for any
// other byte below 0x20; all other bytes pass through unchanged.
inline std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

// The escaped string wrapped in double quotes, ready to drop into a payload.
inline std::string json_string(std::string_view s) {
    return "\"" + json_escape(s) + "\"";
}

}  // namespace pulp::cli
