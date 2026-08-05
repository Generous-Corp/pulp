#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect::discovery_security {

class OwnershipLease {
public:
    ~OwnershipLease();
    static std::unique_ptr<OwnershipLease> acquire(
        const std::filesystem::path& path,
        std::string_view marker);

private:
#ifdef _WIN32
    void* native_handle_ = nullptr;
#else
    int descriptor_ = -1;
#endif
};

bool owner_private_path(const std::filesystem::path& path,
                        bool expect_directory);
std::optional<std::string> read_private_text_file(
    const std::filesystem::path& path);
bool ensure_private_directory(const std::filesystem::path& directory);
bool write_private_file_atomic(const std::filesystem::path& destination,
                               std::string_view contents);

#ifndef _WIN32
bool owner_private_descriptor(int descriptor, bool expect_directory);
int open_owner_private(const std::filesystem::path& path,
                       bool expect_directory);
#endif

} // namespace pulp::inspect::discovery_security
