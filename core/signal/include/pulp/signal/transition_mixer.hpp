#pragma once

/// @file transition_mixer.hpp
/// Shared click-free crossfade primitive.
///
/// The one place the live-swap machinery computes old→new blend gains, so the
/// DSP hot-swap slot, the convolver IR swapper, and the Phase-2 SwapUnit
/// transitions all fade identically and are tested by one fixture. Pure math +
/// a sample position; no allocation, no locks — safe to advance on the audio
/// thread.
///
/// Both laws are evaluated over a smoothstep ramp (zero slope at both ends), so
/// both are click-free at the swap instant AND the fade end; they differ only in
/// the mid-fade sum:
///   - `Smoothstep` — equal-GAIN (old + new == 1). No power bump for correlated
///     old/new (the usual hot-reload: same input, a small DSP tweak). Default.
///   - `EqualPower` — constant POWER (old² + new² == 1), cos/sin. Avoids the
///     mid-fade level dip when old/new are decorrelated (a big DSP change).
///     Mirrors `DryWetMixer`'s EqualPower law.

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::signal {

enum class TransitionCurve { Smoothstep, EqualPower };

template <typename SampleType = float>
class TransitionMixerT {
public:
    TransitionMixerT() = default;

    /// Configure the fade length (samples) + curve and rewind to the start.
    /// length 0 means "no fade" (done() is immediately true).
    void configure(std::size_t length_samples, TransitionCurve curve) {
        length_ = length_samples;
        curve_ = curve;
        pos_ = 0;
    }
    void set_curve(TransitionCurve curve) { curve_ = curve; }
    TransitionCurve curve() const { return curve_; }

    void reset() { pos_ = 0; }
    void advance(std::size_t frames) { pos_ += frames; }

    std::size_t length() const { return length_; }
    std::size_t position() const { return pos_; }
    bool done() const { return length_ == 0 || pos_ >= length_; }

    /// Old/new blend gains at an ABSOLUTE fade position (pure — no state change),
    /// so a caller can compute per-sample gains as `gains_at(position()+n, ...)`.
    /// Evaluated over the smoothstep ramp: click-free ends for both curves.
    void gains_at(std::size_t fade_pos,
                  SampleType& old_gain,
                  SampleType& new_gain) const {
        SampleType t = (length_ == 0)
            ? SampleType{1.0f}
            : static_cast<SampleType>(fade_pos) / static_cast<SampleType>(length_);
        if (t > SampleType{1.0f}) t = SampleType{1.0f};
        const SampleType ramp =
            t * t * (SampleType{3.0f} - SampleType{2.0f} * t);
        if (curve_ == TransitionCurve::EqualPower) {
            constexpr SampleType kHalfPi = SampleType{1.57079632679489661923f};
            const SampleType theta = ramp * kHalfPi;
            old_gain = std::cos(theta);                 // old²+new² == 1
            new_gain = std::sin(theta);
        } else {
            old_gain = SampleType{1.0f} - ramp;         // old+new == 1
            new_gain = ramp;
        }
    }

private:
    std::size_t length_ = 0;
    std::size_t pos_ = 0;
    TransitionCurve curve_ = TransitionCurve::Smoothstep;
};

using TransitionMixer = TransitionMixerT<float>;
using TransitionMixer64 = TransitionMixerT<double>;

/// Blend an already-rendered fading-out buffer into the live output under a
/// TransitionMixer's old->new gains, then advance the mixer by the block.
/// Returns true when the fade has completed. Pure, RT-safe (no alloc/lock);
/// the shared crossfade math for the reload hot-swap slot and the SignalGraph
/// live plugin swap so both stay click-free by the same law. Templated on the
/// mixer + buffer-view types to avoid a signal->audio header dependency; `out`
/// holds the new render on entry and the blended result on return, `old_render`
/// is the fading-out instance's render for this block. Channel/frame counts are
/// the min of the two views. The mixer position is shared across channels
/// (read before the loop, advanced once after).
template <class Mixer, class OutView, class OldView>
inline bool blend_fade_out(Mixer& mixer, OutView& out, const OldView& old_render) noexcept {
    const std::size_t frames = std::min<std::size_t>(out.num_samples(), old_render.num_samples());
    const std::size_t ch = std::min<std::size_t>(out.num_channels(), old_render.num_channels());
    const std::size_t base = mixer.position();
    // The old/new gain pair depends only on the (shared) fade position, not the
    // channel, so compute each pair ONCE per frame and reuse it across channels
    // instead of re-evaluating gains_at (two transcendentals for EqualPower) per
    // channel per sample. Precompute in bounded stack chunks to stay alloc-free
    // while keeping the per-channel apply loop contiguous. Bit-identical to the
    // previous channel-outer form (gains_at is a pure function of base+n).
    constexpr std::size_t kChunk = 64;
    float old_gain[kChunk];
    float new_gain[kChunk];
    for (std::size_t start = 0; start < frames; start += kChunk) {
        const std::size_t len = std::min<std::size_t>(kChunk, frames - start);
        for (std::size_t j = 0; j < len; ++j)
            mixer.gains_at(base + start + j, old_gain[j], new_gain[j]);
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto old_ch = old_render.channel(c);
            for (std::size_t j = 0; j < len; ++j)
                o[start + j] = old_ch[start + j] * old_gain[j] + o[start + j] * new_gain[j];
        }
    }
    mixer.advance(frames);
    return mixer.done();
}

}  // namespace pulp::signal
