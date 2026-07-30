#include "atomic_publisher.hpp"

#include <pulp/timeline/asset_path.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace pulp::tools::timeline::detail {
namespace fs = std::filesystem;

bool publish_path_no_replace(const fs::path& source, const fs::path& destination) noexcept {
#ifdef _WIN32
    return ::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#elif defined(__APPLE__)
    return ::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0;
#elif defined(__linux__)
    return ::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                     RENAME_NOREPLACE) == 0;
#else
    (void)source;
    (void)destination;
    return false;
#endif
}

std::optional<AtomicPublisher> AtomicPublisher::create(const fs::path& destination) {
    if (destination.empty())
        return std::nullopt;
    std::error_code error;
    const auto status = fs::symlink_status(destination, error);
    if ((!error && status.type() != fs::file_type::not_found) ||
        (error && error != std::errc::no_such_file_or_directory))
        return std::nullopt;
    error.clear();
    auto parent = destination.parent_path();
    if (parent.empty())
        parent = fs::current_path(error);
    if (error || !fs::is_directory(parent, error) || error)
        return std::nullopt;

    static std::atomic<std::uint64_t> counter{0};
    const auto seed =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    for (std::uint64_t attempt = 0; attempt < 128; ++attempt) {
        const auto suffix = seed ^ counter.fetch_add(1, std::memory_order_relaxed) ^ attempt;
        auto staging = parent / fs::path(".pulp-staging-" + std::to_string(suffix));
        if (fs::create_directory(staging, error))
            return AtomicPublisher(destination, std::move(staging));
        if (error != std::errc::file_exists)
            return std::nullopt;
        error.clear();
    }
    return std::nullopt;
}

AtomicPublisher::AtomicPublisher(fs::path destination, fs::path staging)
    : destination_(std::move(destination)), staging_(std::move(staging)) {}

AtomicPublisher::AtomicPublisher(AtomicPublisher&& other) noexcept
    : destination_(std::move(other.destination_)), staging_(std::move(other.staging_)),
      committed_(other.committed_) {
    other.committed_ = true;
}

AtomicPublisher::~AtomicPublisher() {
    if (!committed_) {
        std::error_code ignored;
        fs::remove_all(staging_, ignored);
    }
}

bool AtomicPublisher::write(std::string_view relative_utf8, std::span<const std::uint8_t> bytes) {
    if (!pulp::timeline::package_relative_path_is_lexically_safe(relative_utf8))
        return false;
    fs::path relative;
    try {
        relative = filesystem_path_from_utf8(relative_utf8);
    } catch (...) {
        return false;
    }
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory())
        return false;
    const auto output = staging_ / relative;
    std::error_code error;
    if (fs::exists(output, error) || error)
        return false;
    if (!fs::create_directories(output.parent_path(), error) && error)
        return false;
    try {
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        if (!bytes.empty())
            stream.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream)
            return false;
        stream.close();
        return static_cast<bool>(stream);
    } catch (...) {
        return false;
    }
}

bool AtomicPublisher::write(std::string_view relative_utf8, std::string_view text) {
    return write(relative_utf8,
                 std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                               text.size()));
}

bool AtomicPublisher::commit_directory() noexcept {
    if (!publish_path_no_replace(staging_, destination_))
        return false;
    committed_ = true;
    return true;
}

bool AtomicPublisher::commit_file(const fs::path& staged_file) noexcept {
    if (staged_file.parent_path() != staging_ ||
        !publish_path_no_replace(staged_file, destination_))
        return false;
    std::error_code ignored;
    fs::remove(staging_, ignored);
    committed_ = true;
    return true;
}

} // namespace pulp::tools::timeline::detail
