#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace pulp::cli::gpu_artifacts {

// Owns an artifact directory identity for the complete publication window.
// Publication is relative to the pinned identity on both POSIX and Windows, so
// replacing or turning its pathname into a reparse point cannot redirect bytes.
class PinnedArtifactDirectory {
  public:
    static PinnedArtifactDirectory open_or_create(const std::filesystem::path& path);

    ~PinnedArtifactDirectory();
    PinnedArtifactDirectory(PinnedArtifactDirectory&&) noexcept;
    PinnedArtifactDirectory& operator=(PinnedArtifactDirectory&&) noexcept;

    PinnedArtifactDirectory(const PinnedArtifactDirectory&) = delete;
    PinnedArtifactDirectory& operator=(const PinnedArtifactDirectory&) = delete;

    void publish(std::string_view name, std::span<const std::uint8_t> bytes);

  private:
    struct State;
    explicit PinnedArtifactDirectory(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

} // namespace pulp::cli::gpu_artifacts
