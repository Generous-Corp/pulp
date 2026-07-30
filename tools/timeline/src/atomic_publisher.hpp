#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace pulp::tools::timeline::detail {

bool publish_path_no_replace(const std::filesystem::path& source,
                             const std::filesystem::path& destination) noexcept;

class AtomicPublisher {
  public:
    static std::optional<AtomicPublisher> create(const std::filesystem::path& destination);

    AtomicPublisher(AtomicPublisher&& other) noexcept;
    AtomicPublisher& operator=(AtomicPublisher&&) = delete;
    AtomicPublisher(const AtomicPublisher&) = delete;
    AtomicPublisher& operator=(const AtomicPublisher&) = delete;
    ~AtomicPublisher();

    const std::filesystem::path& staging_directory() const noexcept {
        return staging_;
    }
    bool write(std::string_view relative_utf8, std::span<const std::uint8_t> bytes);
    bool write(std::string_view relative_utf8, std::string_view text);
    bool commit_directory() noexcept;
    bool commit_file(const std::filesystem::path& staged_file) noexcept;

  private:
    AtomicPublisher(std::filesystem::path destination, std::filesystem::path staging);

    std::filesystem::path destination_;
    std::filesystem::path staging_;
    bool committed_ = false;
};

} // namespace pulp::tools::timeline::detail
