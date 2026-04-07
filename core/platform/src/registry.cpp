#include <pulp/platform/win/registry.hpp>

#ifdef _WIN32
#include <windows.h>
#include <string>

namespace pulp::platform::win {

static HKEY parse_root(std::string_view root) {
    if (root == "HKCU" || root == "HKEY_CURRENT_USER") return HKEY_CURRENT_USER;
    if (root == "HKLM" || root == "HKEY_LOCAL_MACHINE") return HKEY_LOCAL_MACHINE;
    return HKEY_CURRENT_USER;
}

std::optional<std::string> registry_read_string(
    std::string_view root, std::string_view path, std::string_view name) {
    HKEY key;
    std::string path_str(path);
    if (RegOpenKeyExA(parse_root(root), path_str.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return std::nullopt;

    char buffer[1024];
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    std::string name_str(name);
    LONG result = RegQueryValueExA(key, name_str.c_str(), nullptr, &type,
                                    reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS || type != REG_SZ)
        return std::nullopt;

    return std::string(buffer, size > 0 ? size - 1 : 0);
}

std::optional<uint32_t> registry_read_dword(
    std::string_view root, std::string_view path, std::string_view name) {
    HKEY key;
    std::string path_str(path);
    if (RegOpenKeyExA(parse_root(root), path_str.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return std::nullopt;

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    std::string name_str(name);
    LONG result = RegQueryValueExA(key, name_str.c_str(), nullptr, &type,
                                    reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS || type != REG_DWORD)
        return std::nullopt;

    return value;
}

bool registry_write_string(
    std::string_view root, std::string_view path,
    std::string_view name, std::string_view value) {
    HKEY key;
    std::string path_str(path);
    if (RegCreateKeyExA(parse_root(root), path_str.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    std::string name_str(name);
    std::string value_str(value);
    LONG result = RegSetValueExA(key, name_str.c_str(), 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(value_str.c_str()),
                                  static_cast<DWORD>(value_str.size() + 1));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool registry_write_dword(
    std::string_view root, std::string_view path,
    std::string_view name, uint32_t value) {
    HKEY key;
    std::string path_str(path);
    if (RegCreateKeyExA(parse_root(root), path_str.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    std::string name_str(name);
    LONG result = RegSetValueExA(key, name_str.c_str(), 0, REG_DWORD,
                                  reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool registry_delete_value(
    std::string_view root, std::string_view path, std::string_view name) {
    HKEY key;
    std::string path_str(path);
    if (RegOpenKeyExA(parse_root(root), path_str.c_str(), 0, KEY_WRITE, &key) != ERROR_SUCCESS)
        return false;

    std::string name_str(name);
    LONG result = RegDeleteValueA(key, name_str.c_str());
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

}  // namespace pulp::platform::win

#else  // Non-Windows stubs

namespace pulp::platform::win {

std::optional<std::string> registry_read_string(
    std::string_view, std::string_view, std::string_view) {
    return std::nullopt;
}

std::optional<uint32_t> registry_read_dword(
    std::string_view, std::string_view, std::string_view) {
    return std::nullopt;
}

bool registry_write_string(std::string_view, std::string_view,
                           std::string_view, std::string_view) {
    return false;
}

bool registry_write_dword(std::string_view, std::string_view,
                          std::string_view, uint32_t) {
    return false;
}

bool registry_delete_value(std::string_view, std::string_view, std::string_view) {
    return false;
}

}  // namespace pulp::platform::win

#endif
