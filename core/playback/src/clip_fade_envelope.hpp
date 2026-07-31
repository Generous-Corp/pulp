#pragma once

#include <pulp/playback/audio_renderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace pulp::playback::detail {

/// Maps normalized fade progress to a gain multiplier for the authored shape.
///
/// `progress` runs 0 at the silent edge to 1 at the fully open edge, in both
/// fade directions; callers pass the same normalized quantity for a fade in
/// and a fade out. The linear shape is the identity, so an unshaped fade costs
/// exactly the multiply it always did.
///
/// The shape is a pure reparameterization of `progress` and introduces no time
/// unit of its own. That is what makes tempo mapping a caller concern: the
/// compiler already converts fade endpoints to frames, and progress is measured
/// in frames for every shape, so a shaped fade inherits precisely the tempo
/// behavior the linear ramp already had — no more and no less.
template <typename T> T fade_gain(timeline::ClipFadeShape shape, T progress) noexcept {
    if (shape == timeline::ClipFadeShape::EqualPower)
        return std::sin(progress * std::numbers::pi_v<T> / T{2});
    return progress;
}

/// Clip gain including both fades, for a frame-exact position within the clip.
///
/// `relative` is a whole frame offset from the clip's first timeline frame, so
/// the remaining-frame count is exact integer arithmetic rather than a clamped
/// subtraction.
inline float clip_envelope(const AudioClipRendererProgram& clip,
                           std::uint64_t relative) noexcept {
    float value = clip.gain_linear;
    if (clip.fade_in_frames != 0 && relative < clip.fade_in_frames)
        value *= fade_gain(clip.fade_shape, static_cast<float>(relative) /
                                                static_cast<float>(clip.fade_in_frames));
    const auto remaining = clip.timeline_frame_count - 1u - relative;
    if (clip.fade_out_frames != 0 && remaining < clip.fade_out_frames)
        value *= fade_gain(clip.fade_shape, static_cast<float>(remaining) /
                                                static_cast<float>(clip.fade_out_frames));
    return value;
}

/// Clip gain including both fades, for a fractional position within the clip.
///
/// Used by the paths that read the source at a resampled or stretched position,
/// where the clip-relative position is not a whole frame. `relative` may land
/// fractionally past the last frame, so the remaining-frame count is clamped
/// rather than wrapped.
inline float clip_envelope(const AudioClipRendererProgram& clip, long double relative) noexcept {
    auto value = static_cast<long double>(clip.gain_linear);
    if (clip.fade_in_frames != 0 && relative < clip.fade_in_frames)
        value *= fade_gain(clip.fade_shape,
                           relative / static_cast<long double>(clip.fade_in_frames));
    const auto remaining =
        std::max(0.0L, static_cast<long double>(clip.timeline_frame_count - 1u) - relative);
    if (clip.fade_out_frames != 0 && remaining < clip.fade_out_frames)
        value *= fade_gain(clip.fade_shape,
                           remaining / static_cast<long double>(clip.fade_out_frames));
    return static_cast<float>(value);
}

} // namespace pulp::playback::detail
