#pragma once

#include <pulp/playback/automation_program.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>

#include <cstddef>
#include <cstdint>

namespace pulp::playback {

/// Mirrors timeline::kMaximumTrackGainLinear. Kept here so the render path can
/// bound a streamed curve value without the playback module reaching into the
/// document model for a constant; the static_assert below keeps the two honest.
inline constexpr float kMaximumTrackMixerGain = 64.0f;
static_assert(kMaximumTrackMixerGain == timeline::kMaximumTrackGainLinear,
              "the render path must bound an automated fader by exactly the range the "
              "document accepts for a static one, or a curve could ask for a gain the "
              "model refuses to store");

/// The compiled form of a track's own level and stereo placement. A null lane
/// pointer means the authored constant governs for the whole timeline; a
/// non-null one supersedes it entirely, so a lane and a constant never both
/// apply and the result never depends on which was read first. The pointers
/// borrow from the TrackAutomationProgram the owning TrackProgram holds alive.
struct TrackMixerProgram {
    float gain_linear = 1.0f;
    float pan = 0.0f;
    const AutomationProgram* gain_automation = nullptr;
    const AutomationProgram* pan_automation = nullptr;

    /// Whether this mixer can be skipped entirely. A track that was never
    /// touched renders through exactly the code path it did before mixer state
    /// existed, which is what keeps existing golden renders bit-identical.
    bool transparent() const noexcept {
        return gain_automation == nullptr && pan_automation == nullptr && gain_linear == 1.0f &&
               pan == 0.0f;
    }
};

/// Constrains a streamed control value to the range the document accepts for the
/// same control as a constant. A curve is authored point by point and is not
/// range checked at insert time, so without this a lane could ask for a gain the
/// model refuses to store, and a single NaN point would poison every sample
/// downstream of the track. A value the mixer cannot interpret rests at neutral
/// rather than at an extreme, so a broken curve leaves the track audible and
/// centred instead of silently muting or hard-panning it.
constexpr float clamped_track_gain(float value) noexcept {
    if (!(value >= 0.0f))
        return value <= 0.0f ? 0.0f : 1.0f;
    return value > kMaximumTrackMixerGain ? kMaximumTrackMixerGain : value;
}

constexpr float clamped_track_pan(float value) noexcept {
    if (!(value >= -1.0f))
        return value <= -1.0f ? -1.0f : 0.0f;
    return value > 1.0f ? 1.0f : value;
}

/// Per-side multipliers for a stereo balance. Pan attenuates the opposite side
/// and never boosts, so centre is exactly unity and a hard pan cannot make a
/// track louder than the fader asked for.
struct TrackMixerChannelGains {
    float left = 1.0f;
    float right = 1.0f;
};

constexpr TrackMixerChannelGains track_mixer_channel_gains(float pan) noexcept {
    return {pan > 0.0f ? 1.0f - pan : 1.0f, pan < 0.0f ? 1.0f + pan : 1.0f};
}

/// The balance multiplier for one output channel. A bus narrower than stereo has
/// no opposite side to attenuate, so pan is inert there rather than silently
/// halving a mono track. Wider buses alternate sides by channel parity.
constexpr float track_mixer_channel_gain(float pan, std::size_t channel,
                                         std::size_t channel_count) noexcept {
    if (channel_count < 2)
        return 1.0f;
    const auto gains = track_mixer_channel_gains(pan);
    return channel % 2u == 0u ? gains.left : gains.right;
}

/// Streams one mixer control's value in timeline-sample order without
/// allocating. Positions must be non-decreasing between seeks; a seek is the
/// explicit discontinuity, which is the same contract the device-delivery
/// cursor works under. Evaluation goes through the shared segment evaluator, so
/// an automated fader and an automated plugin parameter read one curve the same
/// way.
class TrackMixerControlCursor {
  public:
    void reset(const AutomationProgram* program, float constant) noexcept {
        program_ = program && !program->empty() ? program : nullptr;
        constant_ = constant;
        segment_index_ = 0;
        cold_ = true;
    }

    /// Whether any position can yield something other than the constant.
    bool automated() const noexcept {
        return program_ != nullptr;
    }

    /// Begins a fresh monotonic run. Callers that revisit earlier positions —
    /// a second channel over the same span, a loop wrap — restart rather than
    /// stepping backwards, which the forward cursor cannot express.
    void restart(const timebase::CompiledTempoMap& map) noexcept {
        if (!program_)
            return;
        tempo_.reset(map);
        segment_index_ = 0;
        cold_ = true;
    }

    float value_at(timebase::SamplePosition sample) noexcept {
        if (!program_)
            return constant_;
        segment_index_ = select_automation_segment(*program_, sample, cold_, segment_index_);
        const auto tick = cold_ ? tempo_.seek(sample).tick : tempo_.advance(sample).tick;
        cold_ = false;
        if (sample < program_->segments().front().start_sample)
            return program_->leading_value();
        return evaluate_automation_segment(*program_, segment_index_, sample, tick);
    }

  private:
    const AutomationProgram* program_ = nullptr;
    float constant_ = 1.0f;
    timebase::TempoCursor tempo_;
    std::size_t segment_index_ = 0;
    bool cold_ = true;
};

} // namespace pulp::playback
