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

/// Gain contributed by the placement fades a flattened leaf sits under, for a
/// position measured in clip-relative frames.
///
/// Each ramp is evaluated as a window of fade PROGRESS, because that is what a
/// shape reparameterizes. A leaf lies inside part of the ramp, so the shape is
/// applied to the progress the leaf actually spans rather than to a ramp of its
/// own between two endpoint gains; the second reading agrees at both edges and
/// bends the wrong way between them for every shape but the identity.
///
/// Progress is a function of the position alone, so two neighbouring leaves cut
/// from one nested window read the same gain at the frame they share and the
/// boundary is inaudible.
///
/// Ramps multiply. A leaf can sit inside the head ramp of one placement and the
/// tail ramp of another it nests within, each with its own shape, and the
/// product of two shapes is not any single shape — which is why this is a list
/// and not one more pair of fields.
template <typename Position>
float placement_fade_gain(const ClipPlacementFadeProgram& program, Position relative) noexcept {
    const auto position = static_cast<double>(relative);
    float value = 1.0f;
    for (const auto& span : program.segments) {
        const auto progress =
            (position - span.silent_frame) / (span.open_frame - span.silent_frame);
        // Fully open is the common case for a leaf that merely neighbours a
        // ramp, and costs no transcendental.
        if (progress >= 1.0)
            continue;
        if (!(progress > 0.0))
            return 0.0f;
        // Narrowed before the shape lookup for the reason the fractional
        // overload below states: it pins EqualPower to `sinf`.
        value *= fade_gain(span.shape, static_cast<float>(progress));
    }
    return value;
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
    // Guarded on presence: an unnested clip, or one under a placement that
    // carries no fade, pays one predictable branch and nothing else.
    if (clip.placement_fade)
        value *= placement_fade_gain(*clip.placement_fade, relative);
    return value;
}

/// Clip gain including both fades, for a fractional position within the clip.
///
/// Used by the paths that read the source at a resampled or stretched position,
/// where the clip-relative position is not a whole frame. `relative` may land
/// fractionally past the last frame, so the remaining-frame count is clamped
/// rather than wrapped.
inline float clip_envelope(const AudioClipRendererProgram& clip, long double relative) noexcept {
    // `relative` is long double because a fractional position needs the
    // precision; the gain does not. Narrowing progress before the shape lookup
    // pins EqualPower to `sinf`, which matters because this runs once per
    // output sample on the realtime stretch path. Left unnarrowed the shape
    // deduces the position type instead: `sin` on arm64, where long double is
    // double, and on x86_64 the 80-bit `sinl`, which is far dearer still. The
    // gain is narrowed to float on return either way, so the wider sin buys
    // nothing.
    auto value = static_cast<long double>(clip.gain_linear);
    if (clip.fade_in_frames != 0 && relative < clip.fade_in_frames)
        value *= fade_gain(clip.fade_shape,
                           static_cast<float>(relative / static_cast<long double>(
                                                             clip.fade_in_frames)));
    const auto remaining =
        std::max(0.0L, static_cast<long double>(clip.timeline_frame_count - 1u) - relative);
    if (clip.fade_out_frames != 0 && remaining < clip.fade_out_frames)
        value *= fade_gain(clip.fade_shape,
                           static_cast<float>(remaining / static_cast<long double>(
                                                              clip.fade_out_frames)));
    if (clip.placement_fade)
        value *= static_cast<long double>(placement_fade_gain(*clip.placement_fade, relative));
    return static_cast<float>(value);
}

} // namespace pulp::playback::detail
