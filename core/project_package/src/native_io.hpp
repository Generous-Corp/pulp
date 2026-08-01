#pragma once

#include <pulp/project_package/atomic_publisher.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pulp::project_package::detail {

template <typename Size> std::optional<Size> checked_size_limit(std::uint64_t value) noexcept {
    static_assert(std::is_unsigned_v<Size>);
    if (value > static_cast<std::uint64_t>(std::numeric_limits<Size>::max()))
        return std::nullopt;
    return static_cast<Size>(value);
}

enum class PackageFaultPoint : std::uint8_t;
enum class NativeReadOutcome : std::uint8_t { Ok, InvalidFile, LimitExceeded, IoError };

class AnchoredDirectory {
  public:
    AnchoredDirectory() noexcept = default;
    ~AnchoredDirectory();
    AnchoredDirectory(AnchoredDirectory&& other) noexcept;
    AnchoredDirectory& operator=(AnchoredDirectory&& other) noexcept;
    AnchoredDirectory(const AnchoredDirectory&) = delete;
    AnchoredDirectory& operator=(const AnchoredDirectory&) = delete;

    static std::optional<AnchoredDirectory> open(const std::filesystem::path& path) noexcept;
    bool write_exclusive_and_fence(const std::filesystem::path& relative,
                                   std::span<const std::uint8_t> bytes,
                                   PackageFaultPoint written_point,
                                   PackageFaultPoint fenced_point) const noexcept;
    void close() noexcept;

  private:
    explicit AnchoredDirectory(std::intptr_t native) noexcept : native_(native) {}
    std::intptr_t native_ = -1;
};

/// One no-follow file handle used for both content verification and durability fencing.
class PinnedFile {
  public:
    PinnedFile() noexcept = default;
    ~PinnedFile();
    PinnedFile(PinnedFile&& other) noexcept;
    PinnedFile& operator=(PinnedFile&& other) noexcept;
    PinnedFile(const PinnedFile&) = delete;
    PinnedFile& operator=(const PinnedFile&) = delete;

    static std::optional<PinnedFile> open(const std::filesystem::path& path,
                                          bool fence_capable) noexcept;
    bool hash_matches(std::string_view expected_hex, std::uint64_t maximum_bytes) const noexcept;
    bool fence() const noexcept;
    /// Returns true only while `path` still names this single-linked regular file.
    bool still_named_by(const std::filesystem::path& path) const noexcept;
    void close() noexcept;

  private:
    explicit PinnedFile(std::intptr_t native) noexcept : native_(native) {}
    std::intptr_t native_ = -1;
};

bool write_exclusive_and_fence(const std::filesystem::path& path,
                               std::span<const std::uint8_t> bytes, PackageFaultPoint written_point,
                               PackageFaultPoint fenced_point) noexcept;
bool fence_file(const std::filesystem::path& path) noexcept;
bool fence_directory(const std::filesystem::path& directory) noexcept;
bool publish_no_replace(const std::filesystem::path& source,
                        const std::filesystem::path& destination) noexcept;
bool replace_path(const std::filesystem::path& source,
                  const std::filesystem::path& destination) noexcept;
bool regular_file_no_links(const std::filesystem::path& path) noexcept;
NativeReadOutcome read_file_bounded(const std::filesystem::path& path, std::uint64_t maximum_bytes,
                                    std::vector<std::uint8_t>& bytes) noexcept;

} // namespace pulp::project_package::detail
