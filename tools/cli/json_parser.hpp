// SPDX-License-Identifier: MIT
// Minimal JSON parser for registry files. Handles objects, arrays,
// strings, numbers, booleans, null. No external dependencies.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace pulp::cli::pkg {

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool bool_val = false;
    double num_val = 0;
    std::string str_val;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* get(const std::string& key) const {
        if (type != Object) return nullptr;
        for (auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }

    std::string as_string() const {
        return type == String ? str_val : std::string{};
    }

    bool as_bool() const { return type == Bool && bool_val; }
    int as_int() const { return type == Number ? static_cast<int>(num_val) : 0; }

    std::vector<std::string> as_string_array() const {
        std::vector<std::string> r;
        if (type == Array)
            for (auto& v : arr)
                if (v.type == String) r.push_back(v.str_val);
        return r;
    }
};

struct JsonParser {
    const std::string& src;
    size_t pos = 0;

    void skip_ws() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' ||
               src[pos] == '\n' || src[pos] == '\r'))
            ++pos;
    }

    char peek() { skip_ws(); return pos < src.size() ? src[pos] : '\0'; }
    char next() { skip_ws(); return pos < src.size() ? src[pos++] : '\0'; }

    std::string parse_string() {
        if (next() != '"') return {};
        std::string r;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return r;
            if (c == '\\' && pos < src.size()) {
                c = src[pos++];
                switch (c) {
                    case '"': r += '"'; break;
                    case '\\': r += '\\'; break;
                    case '/': r += '/'; break;
                    case 'n': r += '\n'; break;
                    case 'r': r += '\r'; break;
                    case 't': r += '\t'; break;
                    case 'u': pos += 4; r += '?'; break;
                    default: r += c;
                }
            } else {
                r += c;
            }
        }
        return r;
    }

    JsonValue parse_value() {
        char c = peek();
        if (c == '"') return {JsonValue::String, false, 0, parse_string(), {}, {}};
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == 't' || c == 'f') {
            bool val = (c == 't');
            pos += val ? 4 : 5;
            return {JsonValue::Bool, val, 0, {}, {}, {}};
        }
        if (c == 'n') { pos += 4; return {}; }
        size_t start = pos;
        skip_ws(); start = pos;
        while (pos < src.size() && (src[pos] >= '0' && src[pos] <= '9' ||
               src[pos] == '-' || src[pos] == '+' || src[pos] == '.' ||
               src[pos] == 'e' || src[pos] == 'E'))
            ++pos;
        double val = 0;
        try { val = std::stod(src.substr(start, pos - start)); } catch (...) {}
        return {JsonValue::Number, false, val, {}, {}, {}};
    }

    JsonValue parse_object() {
        JsonValue r; r.type = JsonValue::Object;
        next();
        if (peek() == '}') { next(); return r; }
        while (true) {
            auto key = parse_string();
            next();
            auto val = parse_value();
            r.obj.push_back({key, val});
            if (peek() == '}') { next(); return r; }
            next();
        }
    }

    JsonValue parse_array() {
        JsonValue r; r.type = JsonValue::Array;
        next();
        if (peek() == ']') { next(); return r; }
        while (true) {
            r.arr.push_back(parse_value());
            if (peek() == ']') { next(); return r; }
            next();
        }
    }

    JsonValue parse() { return parse_value(); }
};

}  // namespace pulp::cli::pkg
