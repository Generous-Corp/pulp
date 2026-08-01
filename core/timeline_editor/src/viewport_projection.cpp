#include <pulp/timeline_editor/viewport_projection.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::timeline_editor {

namespace {

bool finite(PixelSpan pixels) noexcept {
    return std::isfinite(pixels.origin) && std::isfinite(pixels.extent) &&
           std::isfinite(pixels.origin + pixels.extent);
}

float finite_float(long double value) noexcept {
    constexpr auto maximum = static_cast<long double>(std::numeric_limits<float>::max());
    return static_cast<float>(std::clamp(value, -maximum, maximum));
}

} // namespace

runtime::Result<TickProjection, ViewportProjectionError>
TickProjection::create(timebase::TickPosition visible_start,
                       timebase::TickDuration visible_duration, PixelSpan pixels) noexcept {
    if (!finite(pixels))
        return runtime::Err(ViewportProjectionError::NonFinitePixelSpan);
    if (!(pixels.extent > 0.0f) || !(pixels.origin + pixels.extent > pixels.origin))
        return runtime::Err(ViewportProjectionError::NonPositivePixelExtent);
    if (visible_duration.value <= 0)
        return runtime::Err(ViewportProjectionError::NonPositiveTickDuration);
    if (visible_start.value > std::numeric_limits<std::int64_t>::max() - visible_duration.value)
        return runtime::Err(ViewportProjectionError::TickRangeOverflow);

    return runtime::Ok(TickProjection{visible_start, visible_duration, pixels});
}

float TickProjection::x_at(timebase::TickPosition tick) const noexcept {
    const auto delta =
        static_cast<long double>(tick.value) - static_cast<long double>(visible_start_.value);
    const auto fraction = delta / static_cast<long double>(visible_duration_.value);
    return finite_float(static_cast<long double>(pixels_.origin) +
                        fraction * static_cast<long double>(pixels_.extent));
}

timebase::TickPosition TickProjection::tick_at(float x) const noexcept {
    if (std::isnan(x) || x <= pixels_.origin)
        return visible_start_;
    const float pixel_end = pixels_.origin + pixels_.extent;
    if (x >= pixel_end)
        return visible_end();

    const auto fraction =
        static_cast<long double>(x - pixels_.origin) / static_cast<long double>(pixels_.extent);
    const auto tick = static_cast<long double>(visible_start_.value) +
                      fraction * static_cast<long double>(visible_duration_.value);
    return {static_cast<std::int64_t>(std::round(tick))};
}

runtime::Result<PitchProjection, ViewportProjectionError>
PitchProjection::create(std::uint8_t lowest_pitch, std::uint8_t highest_pitch,
                        PixelSpan pixels) noexcept {
    if (!finite(pixels))
        return runtime::Err(ViewportProjectionError::NonFinitePixelSpan);
    if (!(pixels.extent > 0.0f) || !(pixels.origin + pixels.extent > pixels.origin))
        return runtime::Err(ViewportProjectionError::NonPositivePixelExtent);
    if (lowest_pitch > highest_pitch || highest_pitch > 127)
        return runtime::Err(ViewportProjectionError::InvalidPitchRange);

    return runtime::Ok(PitchProjection{lowest_pitch, highest_pitch, pixels});
}

float PitchProjection::row_height() const noexcept {
    const auto row_count = static_cast<float>(highest_pitch_ - lowest_pitch_ + 1);
    return pixels_.extent / row_count;
}

float PitchProjection::y_at(std::uint8_t pitch) const noexcept {
    const auto row = static_cast<float>(static_cast<std::int32_t>(highest_pitch_) -
                                        static_cast<std::int32_t>(pitch));
    return pixels_.origin + (row + 0.5f) * row_height();
}

std::uint8_t PitchProjection::pitch_at(float y) const noexcept {
    if (std::isnan(y) || y <= pixels_.origin)
        return highest_pitch_;
    const float pixel_end = pixels_.origin + pixels_.extent;
    if (y >= pixel_end)
        return lowest_pitch_;

    const auto row = static_cast<std::int32_t>(std::floor((y - pixels_.origin) / row_height()));
    return static_cast<std::uint8_t>(static_cast<std::int32_t>(highest_pitch_) - row);
}

} // namespace pulp::timeline_editor
