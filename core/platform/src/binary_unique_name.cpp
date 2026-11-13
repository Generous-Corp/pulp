#include <pulp/platform/binary_unique_name.hpp>

#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#include <pulp/platform/win32_sane.hpp>
#else
#include <dlfcn.h>
#endif

namespace pulp::platform {

namespace {

std::string hex(std::uintptr_t value) {
    char buffer[2 + sizeof(std::uintptr_t) * 2 + 1];
    std::snprintf(buffer, sizeof buffer, "%llx", static_cast<unsigned long long>(value));
    return buffer;
}

} // namespace

std::string binary_image_token() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&binary_image_token), &module) ||
        module == nullptr)
        return "0";
    return hex(reinterpret_cast<std::uintptr_t>(module));
#elif defined(__EMSCRIPTEN__)
    return "0";
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&binary_image_token), &info) == 0 ||
        info.dli_fbase == nullptr)
        return "0";
    return hex(reinterpret_cast<std::uintptr_t>(info.dli_fbase));
#endif
}

std::string binary_unique_name(std::string_view base) {
    std::string name(base);
    name += '_';
    name += binary_image_token();
    return name;
}

} // namespace pulp::platform
