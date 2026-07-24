#pragma once

#if defined(__linux__)

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

namespace linux_acl_test {

enum class InstallResult {
    Installed,
    Unsupported,
    Failed,
};

inline std::vector<std::uint8_t> access_acl() {
    std::vector<std::uint8_t> bytes;
    const auto append_u16 = [&bytes](std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    };
    const auto append_u32 = [&bytes](std::uint32_t value) {
        for (int byte = 0; byte != 4; ++byte)
            bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
    };
    const auto append_entry = [&](std::uint16_t tag, std::uint16_t permissions,
                                  std::uint32_t id) {
        append_u16(tag);
        append_u16(permissions);
        append_u32(id);
    };
    append_u32(0x0002); // Linux POSIX ACL xattr format version.
    append_entry(0x0001, 0x0006, 0xffff'ffff); // owner: rw-
    append_entry(0x0002, 0x0004,
                 static_cast<std::uint32_t>(::getuid()) + 1); // named user: r--
    append_entry(0x0004, 0x0000, 0xffff'ffff); // owning group: ---
    append_entry(0x0010, 0x0004, 0xffff'ffff); // mask: r--
    append_entry(0x0020, 0x0000, 0xffff'ffff); // other: ---
    return bytes;
}

inline InstallResult install(const std::filesystem::path& path) {
    const auto acl = access_acl();
    constexpr auto name = "system.posix_acl_access";
    if (::setxattr(path.c_str(), name, acl.data(), acl.size(), 0) == 0)
        return InstallResult::Installed;
    if (errno == ENOTSUP || errno == EOPNOTSUPP)
        return InstallResult::Unsupported;
    return InstallResult::Failed;
}

inline std::optional<std::vector<std::uint8_t>> read(const std::filesystem::path& path) {
    constexpr auto name = "system.posix_acl_access";
    const auto size = ::getxattr(path.c_str(), name, nullptr, 0);
    if (size < 0)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (::getxattr(path.c_str(), name, bytes.data(), bytes.size()) != size)
        return std::nullopt;
    return bytes;
}

} // namespace linux_acl_test

#endif
