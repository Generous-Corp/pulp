#pragma once

#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/tick.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::timeline {

// SHA-256 is the durable identity of media bytes. Paths and package locations
// are late-resolution hints and must never participate in identity.
class ContentHash {
  public:
    constexpr ContentHash() noexcept = default;
    static std::optional<ContentHash> from_hex(std::string_view hex) noexcept;

    std::string to_hex() const;
    constexpr bool valid() const noexcept { return valid_; }
    constexpr const std::array<std::uint8_t, 32>& bytes() const noexcept { return bytes_; }
    constexpr auto operator<=>(const ContentHash&) const = default;

  private:
    explicit constexpr ContentHash(std::array<std::uint8_t, 32> bytes) noexcept
        : bytes_(bytes), valid_(true) {}
    std::array<std::uint8_t, 32> bytes_{};
    bool valid_ = false;
};

enum class AssetStoragePolicy : std::uint8_t {
    External,
    Embedded,
    PreferEmbedded,
};

enum class AssetLocatorKind : std::uint8_t {
    PackageRelative,
    ExternalUri,
};

struct AssetLocator {
    AssetLocatorKind kind = AssetLocatorKind::ExternalUri;
    std::string hint;
    constexpr auto operator<=>(const AssetLocator&) const = default;
};

// A representation is the same logical content at another fidelity or in an
// analysis/cache form. Different playable content (for example a separated
// stem) is a new asset and is not a representation.
struct AssetRepresentation {
    std::string role;
    ContentHash content_hash;
    AssetStoragePolicy storage_policy = AssetStoragePolicy::External;
    std::vector<AssetLocator> locators;
};

enum class AudioLoopPointKind : std::uint8_t {
    Manual,
    Automatic,
};

struct AudioLoopPoint {
    std::uint64_t frame = 0;
    AudioLoopPointKind kind = AudioLoopPointKind::Manual;
    constexpr auto operator<=>(const AudioLoopPoint&) const = default;
};

struct AudioFrameRange {
    std::uint64_t start_frame = 0;
    std::uint64_t end_frame = 0;
    constexpr auto operator<=>(const AudioFrameRange&) const = default;
};

// Typed musical metadata attached to sealed audio content. Tempo is derived
// from musical_length, frame_count, and sample_rate instead of being stored as
// a second value that can drift. Frame ranges are half-open [start, end).
struct AudioLoopInfo {
    std::optional<timebase::TickDuration> musical_length;
    timebase::MeterSignature meter{4, 4};
    bool one_shot = false;
    std::optional<std::uint8_t> root_note;
    std::optional<AudioFrameRange> active_range;
    std::vector<AudioLoopPoint> points;
    std::vector<std::string> tags;
    auto operator<=>(const AudioLoopInfo&) const = default;
};

} // namespace pulp::timeline
