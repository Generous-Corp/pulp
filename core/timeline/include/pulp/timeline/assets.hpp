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

/** @addtogroup timeline_model
 * @{
 */

/// Durable SHA-256 identity for sealed media bytes.
///
/// Paths and package locations are resolution hints and never participate in
/// equality. A default-constructed value is invalid.
class ContentHash {
  public:
    constexpr ContentHash() noexcept = default;
    /// Decodes exactly 64 lowercase hexadecimal digits, or returns null on malformed input.
    static std::optional<ContentHash> from_hex(std::string_view hex) noexcept;

    /// Returns the lowercase 64-digit encoding; invalid hashes encode as 64 zeroes.
    std::string to_hex() const;
    /// Returns whether this value was constructed from a complete digest.
    constexpr bool valid() const noexcept { return valid_; }
    /// Borrows the 32 digest bytes for the lifetime of this value.
    constexpr const std::array<std::uint8_t, 32>& bytes() const noexcept { return bytes_; }
    constexpr auto operator<=>(const ContentHash&) const = default;

  private:
    explicit constexpr ContentHash(std::array<std::uint8_t, 32> bytes) noexcept
        : bytes_(bytes), valid_(true) {}
    std::array<std::uint8_t, 32> bytes_{};
    bool valid_ = false;
};

/// Preferred storage relationship between a project and an asset's bytes.
enum class AssetStoragePolicy : std::uint8_t {
    /// Resolve the bytes outside the project package.
    External,
    /// Store the bytes in the project package.
    Embedded,
    /// Prefer packaged bytes while permitting external resolution.
    PreferEmbedded,
};

/// Namespace in which an asset locator hint is interpreted.
enum class AssetLocatorKind : std::uint8_t {
    /// A lexically safe path relative to a project package.
    PackageRelative,
    /// An application-resolved external URI.
    ExternalUri,
};

/// Non-authoritative location hint for resolving content by hash.
struct AssetLocator {
    AssetLocatorKind kind = AssetLocatorKind::ExternalUri;
    std::string hint;
    constexpr auto operator<=>(const AssetLocator&) const = default;
};

/// Alternate fidelity, analysis, or cache form of the same logical asset.
///
/// Different playable content, such as a separated stem, is a distinct asset
/// rather than a representation.
struct AssetRepresentation {
    std::string role;
    ContentHash content_hash;
    AssetStoragePolicy storage_policy = AssetStoragePolicy::External;
    std::vector<AssetLocator> locators;
};

/// Provenance of a suggested audio loop point.
enum class AudioLoopPointKind : std::uint8_t {
    Manual,
    Automatic,
};

/// Frame position at which sealed audio may loop.
struct AudioLoopPoint {
    std::uint64_t frame = 0;
    AudioLoopPointKind kind = AudioLoopPointKind::Manual;
    constexpr auto operator<=>(const AudioLoopPoint&) const = default;
};

/// Half-open frame interval `[start_frame, end_frame)`.
struct AudioFrameRange {
    std::uint64_t start_frame = 0;
    std::uint64_t end_frame = 0;
    constexpr auto operator<=>(const AudioFrameRange&) const = default;
};

/// Typed musical metadata attached to sealed audio content.
///
/// Tempo is derived from `musical_length`, frame count, and sample rate instead
/// of being stored independently. Frame ranges are half-open.
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

/// @}

} // namespace pulp::timeline
