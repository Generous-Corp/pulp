#pragma once

/// @file voice_modulation_sources.hpp
/// Per-voice modulation sources that feed `VoiceModulationBuffer` lanes.
///
/// The destination side of per-voice modulation is shipped:
/// `voice_modulation_buffer.hpp` carries per-voice lanes for gain, pitch, pan,
/// pressure, timbre, and eight aux targets, at constant or audio rate. This
/// bank supplies the missing source side: one LFO and one AHDSR envelope per
/// voice, prepared with fixed memory, with the two voice-scoped policies a
/// single shared source cannot express:
///
/// - free-running versus note-retriggered LFO phase (a free LFO must ignore
///   note-on; a retriggered one must restart phase and lifecycle on it), and
/// - a per-voice phase offset that spreads unison voices evenly around the
///   cycle instead of stacking them in phase.
///
/// Routing, depth automation, and unit conversion stay with the caller: the
/// caller picks which target each source modulates, per render.
///
/// RT contract: `prepare()` and `release()` are control/background-thread
/// calls that allocate. After `prepare()`, `reset()`, `note_on()`,
/// `note_off()`, `append_lfo_lane()`, `append_envelope_lane()`, and
/// `write_voice()` allocate nothing and are audio-thread safe.
///
/// Determinism: output is bit-identical for the same seed, voice index, event
/// order, and block partition. `reset()` is the only reseeding point; voice
/// `i` derives its LFO seed as `seed + i`, and retriggers deliberately do not
/// reseed the random streams (the shipped `pulp::signal::Lfo` contract).
/// Constant-rate lanes publish the block's first sample, so their value is
/// block-partition dependent by definition; audio-rate lanes are
/// partition-invariant.

#include <pulp/audio/instrument_envelope.hpp>
#include <pulp/audio/voice_modulation_buffer.hpp>
#include <pulp/signal/lfo.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::audio {

/// LFO phase policy on note-on.
enum class VoiceLfoPhasePolicy : std::uint8_t {
    /// The LFO runs from `reset()` and ignores note-on. This is the shipped
    /// `pulp::signal::Lfo` free-running contract.
    FreeRunning,
    /// Note-on restarts phase and the lifecycle for that voice. Random
    /// streams are not reseeded, so distinct notes stay distinct.
    Retrigger,
};

struct VoiceLfoSourceSpec {
    bool enabled = false;
    pulp::signal::Lfo::Wave wave = pulp::signal::Lfo::Wave::sine;
    VoiceLfoPhasePolicy phase_policy = VoiceLfoPhasePolicy::Retrigger;
    /// Free-running rate in Hz; clamped into the shipped LFO's legal range.
    double rate_hz = 5.0;
    /// Bipolar output scale. Negative values invert phase. Must be finite.
    float depth = 1.0f;
    /// Spread unison voices evenly around the cycle: voice `i` starts at
    /// phase offset `i / MaximumVoices` cycles.
    bool unison_phase_spread = false;
    /// Base seed; voice `i` uses `seed + i`.
    std::uint32_t seed = 1;
};

struct VoiceEnvelopeSourceSpec {
    bool enabled = false;
    /// AHDSR shape. `sample_rate` is taken from the bank config at
    /// `prepare()`; the value stored here is ignored.
    AhdsrEnvelopeConfig envelope{};
};

struct VoiceModulationSourcesConfig {
    double sample_rate = 48000.0;
    /// Per-block frame capacity; bounds the audio-rate lanes this bank can
    /// render and its constant-rate scratch. Must be at least 1.
    std::uint32_t max_frames = 0;
    VoiceLfoSourceSpec lfo{};
    VoiceEnvelopeSourceSpec envelope{};
};

/// A fixed-capacity bank of per-voice modulation sources.
///
/// `MaximumVoices` sizes both the source instances and the unison phase
/// spread. The bank stores resolved samples; like `VoiceModulationBuffer`,
/// it applies no routing, scaling beyond depth, smoothing, or unit
/// conversion.
template <std::size_t MaximumVoices>
class VoiceModulationSources {
    static_assert(MaximumVoices > 0, "VoiceModulationSources needs at least one voice");
    static_assert(MaximumVoices <= 256, "VoiceModulationSources bounds voice count at 256");
    static constexpr std::uint32_t kMaxFrames = 1u << 20;

public:
    VoiceModulationSources() = default;

    /// Control/background-thread only. Allocates fixed scratch storage and
    /// prepares every voice's sources. Invalid configs leave the previous
    /// state untouched and return false.
    bool prepare(const VoiceModulationSourcesConfig& config) noexcept {
        if (!config_valid(config))
            return false;
        try {
            scratch_.resize(config.max_frames);
        } catch (...) {
            return false;
        }
        config_ = config;
        config_.envelope.envelope.sample_rate = config.sample_rate;
        for (std::size_t i = 0; i < MaximumVoices; ++i) {
            auto& lfo = lfos_[i];
            lfo.set_mode(config.lfo.phase_policy == VoiceLfoPhasePolicy::FreeRunning
                             ? pulp::signal::Lfo::Mode::free
                             : pulp::signal::Lfo::Mode::retrig);
            lfo.set_wave(config.lfo.wave);
            lfo.set_rate_hz(config.lfo.rate_hz);
            lfo.set_seed(config.lfo.seed + static_cast<std::uint32_t>(i));
            lfo.set_phase_offset(config.lfo.unison_phase_spread
                                     ? static_cast<double>(i) / static_cast<double>(MaximumVoices)
                                     : 0.0);
            lfo.prepare(config.sample_rate);
            if (config.envelope.enabled && !envelopes_[i].prepare(config_.envelope.envelope))
                return false;
        }
        prepared_ = true;
        return true;
    }

    /// Control/background-thread only. Invalidates prepared storage.
    void release() noexcept {
        scratch_.clear();
        scratch_.shrink_to_fit();
        prepared_ = false;
    }

    /// RT-safe after prepare. Resets every voice's sources to a fresh start.
    void reset() noexcept {
        for (std::size_t i = 0; i < MaximumVoices; ++i) {
            lfos_[i].reset();
            envelopes_[i].reset();
        }
    }

    bool prepared() const noexcept { return prepared_; }
    const VoiceModulationSourcesConfig& config() const noexcept { return config_; }

    /// RT-safe after prepare. Retunes every voice's LFO, preserving phase and
    /// the lifecycle stage.
    ///
    /// Rate is an automation destination in every synth that has an LFO at all
    /// — it is what a "LFO Rate" knob writes to — so it cannot be reachable
    /// only through `prepare()`. `prepare()` allocates, is control-thread only,
    /// and restarts phase; sweeping a rate knob through it would stall the LFO
    /// at phase 0 for the whole sweep.
    ///
    /// Rejects a non-finite or non-positive rate — the rates `prepare()`
    /// refuses — and reports whether the rate was taken. The shipped LFO
    /// clamps a non-positive rate up into its own legal range rather than
    /// refusing it, so accepting one here would leave `config()` holding a
    /// rate that `prepare(config())` then rejects: the bank and the config it
    /// reports would disagree.
    bool set_lfo_rate_hz(double rate_hz) noexcept {
        if (!prepared() || !std::isfinite(rate_hz) || !(rate_hz > 0.0))
            return false;
        config_.lfo.rate_hz = rate_hz;
        for (auto& lfo : lfos_)
            lfo.set_rate_hz(rate_hz);
        return true;
    }

    /// RT-safe after prepare. Rescales every voice's bipolar LFO output.
    ///
    /// Negative values invert the wave, which is the documented way to express
    /// a source whose convention is the inverse of this one's. Depth is the
    /// other half of a live LFO's automation surface, and has the same reason
    /// as the rate for not living behind `prepare()`.
    ///
    /// Rejects a non-finite depth, leaving the current depth in place, and
    /// reports whether the depth was taken.
    bool set_lfo_depth(float depth) noexcept {
        if (!prepared() || !std::isfinite(depth))
            return false;
        config_.lfo.depth = depth;
        return true;
    }

    /// RT-safe after prepare. Reshapes every voice's LFO, preserving phase and
    /// the lifecycle stage.
    ///
    /// A shape selector is a control a player turns, so it belongs with the
    /// rate and depth rather than behind `prepare()`. Leaving it prepare-only
    /// forces a caller whose shape is authored per patch to re-prepare a bank
    /// the audio thread may be reading from, which is a data race the caller
    /// then has to invent a publication scheme to avoid. Random streams are not
    /// reseeded, so switching away from `sh_random` and back resumes the same
    /// stream rather than replaying it.
    void set_lfo_wave(pulp::signal::Lfo::Wave wave) noexcept {
        if (!prepared())
            return;
        config_.lfo.wave = wave;
        for (auto& lfo : lfos_)
            lfo.set_wave(wave);
    }

    /// RT-safe after prepare. Note-on retriggers that voice's LFO per its
    /// phase policy (free-running ignores it) and restarts its envelope.
    void note_on(std::size_t voice_index) noexcept {
        if (voice_index >= MaximumVoices)
            return;
        lfos_[voice_index].retrigger();
        envelopes_[voice_index].note_on();
    }

    /// RT-safe after prepare. Releases that voice's envelope.
    void note_off(std::size_t voice_index) noexcept {
        if (voice_index >= MaximumVoices)
            return;
        envelopes_[voice_index].note_off();
    }

    /// RT-safe after prepare. Appends the voice's LFO as one lane to a block
    /// the caller has already begun with `VoiceModulationBuffer::begin_block`.
    /// Fails with `InvalidTarget` when the LFO source is disabled.
    VoiceModulationResult append_lfo_lane(std::size_t voice_index,
                                          VoiceModulationBuffer& destination,
                                          VoiceModulationTarget target,
                                          VoiceModulationRate rate,
                                          std::uint32_t frame_count) noexcept {
        if (voice_index >= MaximumVoices)
            return {false, VoiceModulationStatus::InvalidTarget};
        if (!prepared_)
            return {false, VoiceModulationStatus::NotPrepared};
        if (!destination.prepared())
            return {false, VoiceModulationStatus::NotPrepared};
        if (!block_bounds_valid(destination, frame_count))
            return {false, VoiceModulationStatus::InvalidFrameCount};
        if (!config_.lfo.enabled)
            return {false, VoiceModulationStatus::InvalidTarget};
        auto& lfo = lfos_[voice_index];
        const float depth = config_.lfo.depth;
        if (rate == VoiceModulationRate::AudioRate) {
            auto reservation = destination.reserve_audio_rate(target);
            if (!reservation.ok)
                return {false, reservation.status};
            for (std::uint32_t frame = 0; frame < frame_count; ++frame)
                reservation.values[frame] = depth * lfo.next();
            return {true, VoiceModulationStatus::Ok};
        }
        for (std::uint32_t frame = 0; frame < frame_count; ++frame)
            scratch_[frame] = depth * lfo.next();
        return destination.add_constant(target, scratch_[0]);
    }

    /// RT-safe after prepare. Appends the voice's envelope as one lane to a
    /// block the caller has already begun. Fails with `InvalidTarget` when
    /// the envelope source is disabled.
    VoiceModulationResult append_envelope_lane(std::size_t voice_index,
                                               VoiceModulationBuffer& destination,
                                               VoiceModulationTarget target,
                                               VoiceModulationRate rate,
                                               std::uint32_t frame_count) noexcept {
        if (voice_index >= MaximumVoices)
            return {false, VoiceModulationStatus::InvalidTarget};
        if (!prepared_)
            return {false, VoiceModulationStatus::NotPrepared};
        if (!destination.prepared())
            return {false, VoiceModulationStatus::NotPrepared};
        if (!block_bounds_valid(destination, frame_count))
            return {false, VoiceModulationStatus::InvalidFrameCount};
        if (!config_.envelope.enabled)
            return {false, VoiceModulationStatus::InvalidTarget};
        auto& envelope = envelopes_[voice_index];
        if (rate == VoiceModulationRate::AudioRate) {
            auto reservation = destination.reserve_audio_rate(target);
            if (!reservation.ok)
                return {false, reservation.status};
            envelope.render(std::span<float>(reservation.values, frame_count));
            return {true, VoiceModulationStatus::Ok};
        }
        envelope.render(std::span<float>(scratch_.data(), frame_count));
        return destination.add_constant(target, scratch_[0]);
    }

    /// RT-safe after prepare. Convenience owner of the whole block: begins a
    /// block on `destination` and appends one lane per enabled source to
    /// `lfo_target` and `envelope_target`. Both targets must differ and both
    /// sources must be enabled.
    VoiceModulationResult write_voice(std::size_t voice_index,
                                      VoiceModulationBuffer& destination,
                                      VoiceModulationTarget lfo_target,
                                      VoiceModulationTarget envelope_target,
                                      VoiceModulationRate rate,
                                      std::uint32_t frame_count) noexcept {
        if (voice_index >= MaximumVoices)
            return {false, VoiceModulationStatus::InvalidTarget};
        if (!prepared_)
            return {false, VoiceModulationStatus::NotPrepared};
        if (!destination.prepared())
            return {false, VoiceModulationStatus::NotPrepared};
        if (!block_bounds_valid(destination, frame_count))
            return {false, VoiceModulationStatus::InvalidFrameCount};
        if (!config_.lfo.enabled || !config_.envelope.enabled)
            return {false, VoiceModulationStatus::InvalidTarget};
        if (lfo_target == envelope_target)
            return {false, VoiceModulationStatus::DuplicateTarget};
        if (destination.max_lanes() < 2)
            return {false, VoiceModulationStatus::LaneOverflow};
        auto result = destination.begin_block(frame_count);
        if (!result.ok)
            return result;
        result = append_lfo_lane(voice_index, destination, lfo_target, rate, frame_count);
        if (!result.ok)
            return result;
        return append_envelope_lane(voice_index, destination, envelope_target, rate, frame_count);
    }

private:
    static bool config_valid(const VoiceModulationSourcesConfig& config) noexcept {
        if (!(config.sample_rate > 0.0) || !std::isfinite(config.sample_rate))
            return false;
        if (config.max_frames == 0 || config.max_frames > kMaxFrames)
            return false;
        if (!std::isfinite(config.lfo.rate_hz) || !(config.lfo.rate_hz > 0.0))
            return false;
        if (!std::isfinite(config.lfo.depth))
            return false;
        return true;
    }

    static bool block_bounds_valid(const VoiceModulationBuffer& destination,
                                   std::uint32_t frame_count) noexcept {
        return frame_count != 0 && frame_count <= destination.max_frames();
    }

    std::array<pulp::signal::Lfo, MaximumVoices> lfos_;
    std::array<AhdsrEnvelope, MaximumVoices> envelopes_;
    std::vector<float> scratch_;
    VoiceModulationSourcesConfig config_{};
    bool prepared_ = false;
};

}  // namespace pulp::audio
