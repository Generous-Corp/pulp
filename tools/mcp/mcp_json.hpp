// mcp_json.hpp — JSON-RPC 2.0 framing + minimal JSON field extraction
// for the pulp-mcp server.
//
// Extracted from tools/mcp/pulp_mcp.cpp in the 2026-05 Phase 6 (B4)
// refactor — first cut of the MCP typed-registry split. The pulp-mcp
// server deliberately avoids an external JSON dependency; these are the
// minimal building blocks for emitting JSON-RPC envelopes and pulling
// scalar fields out of an incoming request.
//
// Helpers are header-inline so the framing layer can be shared by the
// protocol handler and (in later B4 cuts) the per-domain tool modules
// without an extra TU or ODR concern.

#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace pulp_mcp {

// ── JSON emit ────────────────────────────────────────────────────────────────

// Escape + quote a string as a JSON string literal.
inline std::string json_string(const std::string& s) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    const auto byte = static_cast<unsigned char>(c);
                    out += "\\u00";
                    out += hex[(byte >> 4) & 0x0f];
                    out += hex[byte & 0x0f];
                } else {
                    out += c;
                }
                break;
        }
    }
    out += "\"";
    return out;
}

// JSON-RPC 2.0 error envelope.
inline std::string json_error(const std::string& id, int code, const std::string& msg) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id +
           ",\"error\":{\"code\":" + std::to_string(code) +
           ",\"message\":" + json_string(msg) + "}}";
}

// JSON-RPC 2.0 result envelope.
inline std::string json_result(const std::string& id, const std::string& result_json) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result_json + "}";
}

// MCP tool-call result payload: a text content block plus the raw
// structured JSON under structuredContent.
inline std::string json_tool_payload(const std::string& structured_json) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(structured_json) +
           "}],\"structuredContent\":" + structured_json + "}";
}

// ── JSON field extraction (simple, dependency-free parsing) ──────────────────

// Pull a string value for `key` out of a flat JSON object. Returns empty
// on any miss. Honors backslash-escaped quotes inside the value.
inline std::string extract_string(const std::string& json, const std::string& key) {
    auto key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return {};

    auto colon = json.find(':', key_pos + key.size() + 2);
    if (colon == std::string::npos) return {};

    auto start = colon + 1;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
    if (start >= json.size() || json[start] != '"') return {};

    auto end = json.find('"', start + 1);
    while (end != std::string::npos && json[end - 1] == '\\') {
        end = json.find('"', end + 1);
    }
    if (end == std::string::npos) return {};

    return json.substr(start + 1, end - start - 1);
}

// Pull the raw token (string-with-quotes, number, or null) for `key`.
inline std::string extract_raw(const std::string& json, const std::string& key) {
    auto key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return {};

    auto colon = json.find(':', key_pos + key.size() + 2);
    if (colon == std::string::npos) return {};

    // Skip whitespace
    auto start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;

    if (start >= json.size()) return {};

    // If it's a string
    if (json[start] == '"') {
        auto end = json.find('"', start + 1);
        while (end != std::string::npos && json[end - 1] == '\\')
            end = json.find('"', end + 1);
        if (end == std::string::npos) return {};
        return json.substr(start, end - start + 1);
    }

    // If it's a number or null
    auto end = json.find_first_of(",}", start);
    if (end == std::string::npos) end = json.size();
    auto value = json.substr(start, end - start);
    // Trim
    while (!value.empty() && (value.back() == ' ' || value.back() == '\n')) value.pop_back();
    return value;
}

// Typed extractors — fall back to `fallback` on miss / parse failure.
inline int extract_int(const std::string& json, const std::string& key, int fallback) {
    auto raw = extract_raw(json, key);
    if (raw.empty() || raw == "null") return fallback;
    try {
        std::size_t pos = 0;
        int value = std::stoi(raw, &pos);
        while (pos < raw.size()) {
            if (!std::isspace(static_cast<unsigned char>(raw[pos]))) return fallback;
            ++pos;
        }
        return value;
    } catch (...) {
        return fallback;
    }
}

inline double extract_double(const std::string& json, const std::string& key, double fallback) {
    auto raw = extract_raw(json, key);
    if (raw.empty() || raw == "null") return fallback;
    try {
        std::size_t pos = 0;
        double value = std::stod(raw, &pos);
        while (pos < raw.size()) {
            if (!std::isspace(static_cast<unsigned char>(raw[pos]))) return fallback;
            ++pos;
        }
        return value;
    } catch (...) {
        return fallback;
    }
}

inline bool extract_bool(const std::string& json, const std::string& key, bool fallback) {
    auto raw = extract_raw(json, key);
    if (raw.empty() || raw == "null") return fallback;
    return raw == "true" ? true : raw == "false" ? false : fallback;
}

}  // namespace pulp_mcp
