#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace pulp::timeline::detail {

class NativeFile {
  public:
    NativeFile() = default;
    explicit NativeFile(int descriptor) noexcept;
    ~NativeFile();
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    NativeFile(NativeFile&& other) noexcept;
    NativeFile& operator=(NativeFile&& other) noexcept;

    static NativeFile open_existing(const std::filesystem::path& path) noexcept;
    static NativeFile open_or_create(const std::filesystem::path& path) noexcept;

    bool valid() const noexcept;
    int native_descriptor() const noexcept;
    void close() noexcept;
    bool seek(std::uint64_t offset) noexcept;
    bool read_all(std::span<std::uint8_t> output) noexcept;
    bool write_all(std::span<const std::uint8_t> input) noexcept;
    std::optional<std::uint64_t> size() const noexcept;
    std::optional<std::uint64_t> link_count() const noexcept;
    bool matches_path(const std::filesystem::path& path) const noexcept;
    bool truncate(std::uint64_t size) noexcept;
    bool sync() noexcept;
    bool lock_exclusive() noexcept;

  private:
    int descriptor_ = -1;
};

bool ensure_parent_directory(const std::filesystem::path& path);
bool resolve_journal_path(const std::filesystem::path& requested, std::filesystem::path& resolved);

} // namespace pulp::timeline::detail
