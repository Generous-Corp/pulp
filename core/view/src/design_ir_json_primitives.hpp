// design_ir_json_primitives.hpp — PRIVATE readers and writers the DesignIR
// JSON translation units share: field accessors over a parsed choc value, and
// member writers that emit `"key":value` pairs with the comma bookkeeping.
// Kept in one place so every IR JSON TU formats numbers and strings the same.
#pragma once

#include "design_import_internal.hpp"

#include <choc/containers/choc_Value.h>

#include <charconv>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace pulp::view::ir_json {

inline std::string get_string(const choc::value::ValueView& obj, const char* key,
                              const char* def = "") {
    if (obj.hasObjectMember(key))
        return std::string(obj[key].toString());
    return def;
}

inline float get_float(const choc::value::ValueView& obj, const char* key, float def = 0.0f) {
    if (obj.hasObjectMember(key))
        return static_cast<float>(obj[key].getWithDefault<double>(def));
    return def;
}

inline bool get_bool(const choc::value::ValueView& obj, const char* key, bool def = false) {
    if (obj.hasObjectMember(key))
        return obj[key].getWithDefault<bool>(def);
    return def;
}

inline void write_key(std::ostringstream& out, bool& first, const char* key) {
    if (!first)
        out << ',';
    first = false;
    out << '"' << key << "\":";
}

inline void write_string_member(std::ostringstream& out, bool& first, const char* key,
                                std::string_view value) {
    write_key(out, first, key);
    out << '"' << json_escape(value) << '"';
}

inline void write_string_member(std::ostringstream& out, bool& first, const char* key,
                                const char* value) {
    write_string_member(out, first, key, std::string_view(value ? value : ""));
}

inline void write_string_member(std::ostringstream& out, bool& first, const char* key,
                                const std::string& value) {
    write_string_member(out, first, key, std::string_view(value));
}

inline void write_string_member(std::ostringstream& out, bool& first, const char* key,
                                const std::optional<std::string>& value) {
    if (value)
        write_string_member(out, first, key, *value);
}

// Shortest text that reads back as the same float. A stream's default six
// significant digits would cut Chromium's 1/64 px layout to 0.1 px from 1000 px up.
inline void write_float_member(std::ostringstream& out, bool& first, const char* key, float value) {
    write_key(out, first, key);
    char buffer[32];
    const auto written = std::to_chars(buffer, buffer + sizeof buffer, value);
    out.write(buffer, written.ptr - buffer);
}

inline void write_float_member(std::ostringstream& out, bool& first, const char* key,
                               const std::optional<float>& value) {
    if (value)
        write_float_member(out, first, key, *value);
}

inline void write_int_member(std::ostringstream& out, bool& first, const char* key, int value) {
    write_key(out, first, key);
    out << value;
}

inline void write_int_member(std::ostringstream& out, bool& first, const char* key,
                             const std::optional<int>& value) {
    if (value)
        write_int_member(out, first, key, *value);
}

} // namespace pulp::view::ir_json
