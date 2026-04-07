#pragma once

// Windows Registry read/write (Windows only — no-ops on other platforms)

#include <string>
#include <string_view>
#include <optional>
#include <cstdint>

namespace pulp::platform::win {

/// Read a string value from the registry.
/// root should be "HKCU" or "HKLM".
/// path is the subkey path (e.g., "Software\\Pulp").
/// name is the value name.
std::optional<std::string> registry_read_string(
    std::string_view root, std::string_view path, std::string_view name);

/// Read a DWORD value from the registry.
std::optional<uint32_t> registry_read_dword(
    std::string_view root, std::string_view path, std::string_view name);

/// Write a string value to the registry. Returns true on success.
bool registry_write_string(
    std::string_view root, std::string_view path,
    std::string_view name, std::string_view value);

/// Write a DWORD value to the registry. Returns true on success.
bool registry_write_dword(
    std::string_view root, std::string_view path,
    std::string_view name, uint32_t value);

/// Delete a value from the registry. Returns true on success.
bool registry_delete_value(
    std::string_view root, std::string_view path, std::string_view name);

}  // namespace pulp::platform::win
