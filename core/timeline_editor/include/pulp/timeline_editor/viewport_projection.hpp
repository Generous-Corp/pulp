#pragma once

#include <cstdint>

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/tick.hpp>

namespace pulp::timeline_editor {

/// A one-dimensional pixel range owned by the editor kernel.
///
/// Keeping this scalar avoids coupling projection math to any view-layer
/// rectangle or pointer type. `extent` is the distance from the leading edge
/// and must be finite and positive.
struct PixelSpan {
    float origin = 0.0f;
    float extent = 0.0f;

    constexpr bool operator==(const PixelSpan&) const = default;
};

enum class ViewportProjectionError : std::uint8_t {
    NonFinitePixelSpan,
    NonPositivePixelExtent,
    NonPositiveTickDuration,
    TickRangeOverflow,
    InvalidPitchRange,
};

/// Projects a visible musical-time range onto a horizontal pixel span.
///
/// Forward projection is intentionally not clipped: renderers need offscreen
/// points to preserve the slope of lines crossing a viewport edge. The inverse
/// is a pointer lookup and clamps to the visible range before rounding to the
/// nearest canonical tick.
class TickProjection {
  public:
    static runtime::Result<TickProjection, ViewportProjectionError>
    create(timebase::TickPosition visible_start, timebase::TickDuration visible_duration,
           PixelSpan pixels) noexcept;

    [[nodiscard]] float x_at(timebase::TickPosition tick) const noexcept;
    [[nodiscard]] timebase::TickPosition tick_at(float x) const noexcept;

    [[nodiscard]] constexpr timebase::TickPosition visible_start() const noexcept {
        return visible_start_;
    }
    [[nodiscard]] constexpr timebase::TickDuration visible_duration() const noexcept {
        return visible_duration_;
    }
    [[nodiscard]] constexpr timebase::TickPosition visible_end() const noexcept {
        return {visible_start_.value + visible_duration_.value};
    }
    [[nodiscard]] constexpr PixelSpan pixels() const noexcept {
        return pixels_;
    }

  private:
    constexpr TickProjection(timebase::TickPosition visible_start,
                             timebase::TickDuration visible_duration, PixelSpan pixels) noexcept
        : visible_start_(visible_start), visible_duration_(visible_duration), pixels_(pixels) {}

    timebase::TickPosition visible_start_{};
    timebase::TickDuration visible_duration_{};
    PixelSpan pixels_{};
};

/// Projects an inclusive MIDI pitch range onto piano-roll rows.
///
/// Screen y increases downward, so the highest pitch owns the first row. A
/// pitch projects to the center of its row without clipping, so renderers can
/// cull notes outside the visible range. Inverse lookup returns the visible row
/// that contains y and clamps outside input to the visible pitch endpoints.
class PitchProjection {
  public:
    static runtime::Result<PitchProjection, ViewportProjectionError>
    create(std::uint8_t lowest_pitch, std::uint8_t highest_pitch, PixelSpan pixels) noexcept;

    [[nodiscard]] float y_at(std::uint8_t pitch) const noexcept;
    [[nodiscard]] std::uint8_t pitch_at(float y) const noexcept;
    [[nodiscard]] float row_height() const noexcept;

    [[nodiscard]] constexpr std::uint8_t lowest_pitch() const noexcept {
        return lowest_pitch_;
    }
    [[nodiscard]] constexpr std::uint8_t highest_pitch() const noexcept {
        return highest_pitch_;
    }
    [[nodiscard]] constexpr PixelSpan pixels() const noexcept {
        return pixels_;
    }

  private:
    constexpr PitchProjection(std::uint8_t lowest_pitch, std::uint8_t highest_pitch,
                              PixelSpan pixels) noexcept
        : lowest_pitch_(lowest_pitch), highest_pitch_(highest_pitch), pixels_(pixels) {}

    std::uint8_t lowest_pitch_ = 0;
    std::uint8_t highest_pitch_ = 127;
    PixelSpan pixels_{};
};

} // namespace pulp::timeline_editor
