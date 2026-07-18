// SPDX-License-Identifier: MIT
#pragma once

// Shared, side-effect-free JSON/text accessors used across the kit_commands
// translation units. These helpers are intentionally header-inline so both
// kit_commands.cpp (policy/apply/archive/dispatch) and
// kit_manifest_validation.cpp can share one definition without linkage games.
// See tools/cli/KIT_COMMANDS_MODULE_MAP.md for the module boundary.

#include "json_parser.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace pulp::cli::kit {

namespace fs = std::filesystem;

using JsonValue = pulp::cli::pkg::JsonValue;
using JsonParser = pulp::cli::pkg::JsonParser;

inline std::string read_text(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

inline std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

inline const JsonValue* object_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Object ? field : nullptr;
}

inline const JsonValue* array_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Array ? field : nullptr;
}

inline std::string string_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::String ? field->str_val : std::string{};
}

inline bool bool_field(const JsonValue& value, const std::string& key, bool fallback = false) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Bool ? field->bool_val : fallback;
}

inline std::vector<std::string> string_array_field(const JsonValue& value, const std::string& key) {
    if (auto* field = array_field(value, key)) return field->as_string_array();
    return {};
}

inline bool has_field(const JsonValue& value, const std::string& key) {
    return value.get(key) != nullptr;
}

inline bool field_array_empty(const JsonValue& object, const std::string& key) {
    auto* value = object.get(key);
    return !value || value->type != JsonValue::Array || value->arr().empty();
}

inline bool vector_contains(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

}  // namespace pulp::cli::kit
