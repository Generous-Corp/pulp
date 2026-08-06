#pragma once

#include <pulp/timeline/assets.hpp>

#include <compare>
#include <cstdint>
#include <optional>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// How a document states the mapping from note number to pitch.
///
/// The document names a tuning; it does not implement one. `core/midi` owns the
/// providers that realize each of these at play time.
enum class TuningSystem : std::uint8_t {
    /// Twelve equal divisions of the octave against the stated reference pitch.
    EqualTemperament,
    /// Defer to whichever MTS-ESP master is present when the document plays.
    ///
    /// The tuning itself lives outside the document by design: an MTS-ESP
    /// master is a live session-wide source, so a document that stores its
    /// current table would pin a snapshot the user did not author.
    MtsEsp,
    /// A sealed Scala scale, and optionally a sealed keyboard mapping.
    Scala,
};

/// Reference pitch for A4 that a document states when it states nothing else.
inline constexpr std::uint32_t kDefaultReferencePitchMillihertz = 440'000;
/// Lowest reference pitch a document may state, in millihertz.
inline constexpr std::uint32_t kMinReferencePitchMillihertz = 20'000;
/// Highest reference pitch a document may state, in millihertz.
inline constexpr std::uint32_t kMaxReferencePitchMillihertz = 20'000'000;

/// Durable statement of the tuning a project or one instrument plays in.
///
/// The reference pitch is exact millihertz rather than a float so a saved
/// document states the pitch the user typed. A tuning is content-addressed
/// rather than identity-addressed: the same scale referenced by two documents
/// is the same bytes, and a copy or import carries the reference without any
/// identity to remap.
struct TuningReference {
    TuningSystem system = TuningSystem::EqualTemperament;
    std::uint32_t reference_pitch_millihertz = kDefaultReferencePitchMillihertz;
    /// Sealed `.scl` payload. Required when `system` is Scala, absent otherwise.
    std::optional<ContentHash> scale_content;
    /// Sealed `.kbm` payload. Only a Scala tuning may name one; absence means
    /// the scale maps onto the keyboard by its own default.
    std::optional<ContentHash> keyboard_map_content;

    constexpr auto operator<=>(const TuningReference&) const = default;
};

/// Returns whether a tuning reference is internally consistent.
///
/// A non-Scala system carrying a payload hash is rejected rather than having
/// the hash ignored: a reader that ignored it would play a different scale
/// from the one the document names.
constexpr bool valid_tuning_reference(const TuningReference& tuning) noexcept {
    if (tuning.reference_pitch_millihertz < kMinReferencePitchMillihertz ||
        tuning.reference_pitch_millihertz > kMaxReferencePitchMillihertz)
        return false;
    switch (tuning.system) {
    case TuningSystem::EqualTemperament:
    case TuningSystem::MtsEsp:
        return !tuning.scale_content && !tuning.keyboard_map_content;
    case TuningSystem::Scala:
        return tuning.scale_content && tuning.scale_content->valid() &&
               (!tuning.keyboard_map_content || tuning.keyboard_map_content->valid());
    }
    return false;
}

/// @}

} // namespace pulp::timeline
