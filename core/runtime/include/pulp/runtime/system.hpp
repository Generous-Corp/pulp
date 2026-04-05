#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::runtime {

inline std::optional<std::string> get_env(std::string_view name) {
    std::string key{name};
#ifdef _WIN32
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, key.c_str()) != 0 || value == nullptr)
        return std::nullopt;

    std::string result(value);
    std::free(value);
    if (result.empty()) return std::nullopt;
    return result;
#else
    if (const char* value = std::getenv(key.c_str()); value && *value)
        return std::string(value);
    return std::nullopt;
#endif
}

inline std::tm gmtime_utc(std::time_t value) {
    std::tm out{};
#ifdef _WIN32
    gmtime_s(&out, &value);
#else
    if (gmtime_r(&value, &out) == nullptr)
        out = {};
#endif
    return out;
}

inline std::tm localtime_local(std::time_t value) {
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &value);
#else
    if (localtime_r(&value, &out) == nullptr)
        out = {};
#endif
    return out;
}

inline void copy_c_string(char* dest, std::size_t dest_size, std::string_view src) {
    if (dest == nullptr || dest_size == 0) return;
    const auto count = (std::min)(src.size(), dest_size - 1);
    std::memcpy(dest, src.data(), count);
    dest[count] = '\0';
}

template <std::size_t N>
inline void copy_c_string(char (&dest)[N], std::string_view src) {
    copy_c_string(dest, N, src);
}

} // namespace pulp::runtime
