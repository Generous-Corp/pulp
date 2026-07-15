#pragma once

/// @file realtime_pitch_time_processor.hpp
/// Realtime pitch shifting and time-scale modification for coherent
/// channel groups, built on SpectralFrameEngine (STFT/WOLA),
/// MultichannelPhaseCoordinator (Laroche-Dolson phase propagation with
/// identity peak locking) and SpectralEnvelopeShifter (formant
/// preservation / shifting).
///
/// Pitch shifting is realized as time-scale modification followed by
/// resampling (Laroche & Dolson, "New Phase-Vocoder Techniques for
/// Pitch-Shifting, Harmonizing and Other Exotic Effects," WASPAA 1999):
/// the synthesis hop tracks `pitch_ratio * analysis_hop` through a
/// fractional accumulator, and an internal Catmull-Rom reader maps each
/// output sample to its stretched-stream position through the producer's
/// own frame map (input frame position -> synthesized start), so
/// process() emits exactly as many samples as it consumes with a fixed,
/// exactly-reported latency at every ratio — the reader cannot drift
/// against the synthesis hops by construction.
///
/// Two modes, chosen at prepare():
///   - `realtime_pitch`: equal-length process(); pitch in semitones,
///     duration preserved. Latency = fft_size + analysis_hop, exact and
///     block-size independent.
///   - `time_stretch`: pull-style feed()/read_stretched(); time ratio
///     independent of pitch (pitch fixed at 0 in this mode). Output
///     availability is tracked exactly; `achieved_time_ratio()` reports
///     the hop-quantized ratio actually applied for test assertions.
///
/// Geometry is derived from the quality mode and the COLA conditions of
/// the Hann window only (quality: 4096/512 at a 96 ms latency budget;
/// low_latency: 1024/256 at 26.7 ms @ 48 kHz) — constants come from the
/// algorithm's own geometry, not from any reference product.
///
/// No allocation or locks after prepare().

#include <pulp/signal/freeze_hold.hpp>
#include <pulp/signal/latency_aware_control_smoother.hpp>
#include <pulp/signal/multichannel_phase_coordinator.hpp>
#include <pulp/signal/noise_morpher.hpp>
#include <pulp/signal/sinc_resampler.hpp>
#include <pulp/signal/spectral_envelope_shifter.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>
#include <pulp/signal/stn_decomposer.hpp>
#include <pulp/signal/transient_phase_policy.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace pulp::signal {

enum class PitchTimeQuality { quality, low_latency };
enum class PitchTimeMode { realtime_pitch, time_stretch };
enum class FormantMode { follow, preserve };

struct RealtimePitchTimeConfig {
    PitchTimeMode mode = PitchTimeMode::realtime_pitch;
    PitchTimeQuality quality = PitchTimeQuality::quality;
    int channels = 1;
    int max_block = 4096;
    float max_pitch_semitones = 12.0f;     // sizing bound, |pitch| clamp
    float max_time_ratio = 2.0f;           // sizing bound for time_stretch
    float pitch_smoothing_seconds = 0.03f; // semitone-domain attack/release
    FormantMode formant_mode = FormantMode::follow;
    int true_envelope_iterations = 3;      // formant-path quality
    /// Phase reset at detected transients (Röbel DAFx 2003) — keeps
    /// attacks sharp under shift/stretch.
    bool transient_preservation = true;
    /// Transient-detector sensitivity (0 = engine default 1.0). Higher fires the
    /// Röbel phase reset on more onsets, so percussion keeps EVERY hit sharp
    /// (a conservative detector softens some hits -> "transients move around").
    /// OfflineStretch raises this for percussive material.
    float transient_sensitivity = 0.0f;
    /// Route the noise component (STN decomposition) through NoiseMorpher
    /// instead of phase propagation, so stretched/shifted noise and
    /// textures stay natural instead of "tonalizing." Adds an STN pass per
    /// frame; off by default to keep the baseline path unchanged.
    bool noise_morphing = false;
    /// Read the stretched stream with a Kaiser-windowed sinc kernel instead
    /// of Catmull-Rom cubic — far deeper stopband, so the resample step of
    /// pitch shifting folds back much less aliasing on large shifts and
    /// bright material. Costs 2*half taps per output sample; off by default.
    bool sinc_resampling = false;
    /// Optional STFT geometry override (offline use). 0 = derive from `quality`
    /// (4096/512). Set both to force a window/overlap suited to the material:
    /// large windows (e.g. 8192/512 = 16× overlap) resolve closely-spaced low
    /// partials so bass stretches without wobble; small windows (1024/128) give
    /// the time resolution that keeps percussive transients natural. `fft_size`
    /// must be a power of two ≥ 256; `analysis_hop` must divide it for COLA.
    /// The realtime path leaves these 0; OfflineStretch picks them per input.
    int fft_size = 0;
    int analysis_hop = 0;
};

template <typename SampleType = float>
class RealtimePitchTimeProcessorT {
public:
    RealtimePitchTimeProcessorT() = default;

    /// RT contract: prepare() allocates and sizes all spectral, smoothing,
    /// ring, drain, noise-morphing, and optional sinc-resampling storage; it is
    /// not audio-thread safe. After prepare(), process(), feed(),
    /// read_stretched(), reset(), control setters, and accessors are
    /// allocation-free for blocks no larger than config.max_block and the
    /// prepared channel count.
    void prepare(double sample_rate, const RealtimePitchTimeConfig& config) {
        assert(sample_rate > 0.0);
        assert(config.channels >= 1);
        config_ = config;
        sample_rate_ = sample_rate;

        assert(config.channels <= kMaxChannels);
        const bool quality = config.quality == PitchTimeQuality::quality;
        fft_size_ = quality ? 4096 : 1024;
        analysis_hop_ = quality ? 512 : 256;
        // Offline geometry override (material-adaptive window/overlap). Only
        // honored when both are set and valid; the realtime path leaves them 0.
        if (config.fft_size >= 256
            && (config.fft_size & (config.fft_size - 1)) == 0
            && config.analysis_hop > 0
            && (config.fft_size % config.analysis_hop) == 0) {
            fft_size_ = config.fft_size;
            analysis_hop_ = config.analysis_hop;
        }

        const float max_ratio = std::exp2(config.max_pitch_semitones / 12.0f);
        const float max_stretch =
            config.mode == PitchTimeMode::time_stretch
                ? std::max(config.max_time_ratio, 1.0f)
                : max_ratio;

        SpectralFrameEngineConfig engine_config;
        engine_config.fft_size = fft_size_;
        engine_config.analysis_hop = analysis_hop_;
        engine_config.channels = config.channels;
        engine_config.max_block = std::max(config.max_block, analysis_hop_);
        engine_config.max_synthesis_hop =
            static_cast<int>(std::ceil(max_stretch * analysis_hop_)) + 1;
        engine_.prepare(engine_config);

        coordinator_.prepare(fft_size_, config.channels);

        typename TransientPhasePolicyT<SampleType>::Config transient_config;
        transient_config.fft_size = fft_size_;
        if (config.transient_sensitivity > 0.0f)
            transient_config.sensitivity = config.transient_sensitivity;
        transient_.prepare(transient_config);

        typename FreezeHoldT<SampleType>::Config freeze_config;
        freeze_config.fft_size = fft_size_;
        freeze_config.channels = config.channels;
        freeze_config.analysis_hop = analysis_hop_;
        freeze_.prepare(freeze_config);

        SpectralEnvelopeShifterConfig env_config;
        env_config.fft_size = fft_size_;
        env_config.true_envelope_iterations = config.true_envelope_iterations;
        envelope_.prepare(env_config);

        LatencyAwareControlSmoother::Config smoother_config;
        smoother_config.domain = LatencyAwareControlSmoother::Domain::semitone;
        smoother_config.attack_seconds = config.pitch_smoothing_seconds;
        smoother_config.release_seconds = config.pitch_smoothing_seconds;
        pitch_smoother_.prepare(sample_rate, smoother_config);
        formant_smoother_.prepare(sample_rate, smoother_config);

        // Stretched-stream ring: must span the read-to-write gap
        // (latency * ratio) plus one engine drain burst and one block.
        const float span = static_cast<float>(fft_size_ + 2 * analysis_hop_
                                              + engine_config.max_block)
                           * std::max(max_stretch, 1.0f)
                         + static_cast<float>(fft_size_) + 64.0f;
        ring_size_ = 1;
        while (ring_size_ < static_cast<int>(span)) ring_size_ <<= 1;
        ring_mask_ = ring_size_ - 1;
        stretch_ring_.assign(static_cast<size_t>(config.channels) * ring_size_, SampleType{0});

        drain_buf_.assign(static_cast<size_t>(config.channels)
                          * engine_config.max_synthesis_hop * 4, SampleType{0});
        drain_ptrs_.resize(static_cast<size_t>(config.channels));

        // Noise-morphing front end: STN decomposition over the spectrum
        // plus one NoiseMorpher per channel. The STN mask is computed from
        // channel 0 and shared across channels for a coherent split.
        const int spectral_bins = fft_size_ / 2 + 1;
        if (config.noise_morphing) {
            StnConfig stn_config;
            stn_config.num_bins = spectral_bins;
            stn_config.time_median = quality ? 7 : 5;
            stn_config.freq_median = quality ? 11 : 7;
            // The morph split applies the mask to the frame it just pushed, so
            // align the mask to the newest frame. Centered masks lag by
            // (time_median-1)/2 frames and misroute transient-onset energy into
            // the noise path (decohering + level-losing percussive hits).
            stn_config.causal = true;
            stn_.prepare(stn_config);
            noise_morphers_.resize(static_cast<size_t>(config.channels));
            // Same seed across channels → coherent (mono-safe) noise phase:
            // identical input yields identical output and the noise sums
            // correctly to mono. The per-channel magnitude envelopes still
            // give each channel its own color.
            for (int ch = 0; ch < config.channels; ++ch)
                noise_morphers_[static_cast<size_t>(ch)].prepare(spectral_bins);
            mag_scratch_.assign(static_cast<size_t>(spectral_bins), SampleType{0});
            noise_env_.assign(static_cast<size_t>(config.channels) * spectral_bins, SampleType{0});
            noise_spec_.assign(static_cast<size_t>(spectral_bins), std::complex<SampleType>{});
        }
        if (config.sinc_resampling) {
            resampler_.build();
            tap_scratch_.assign(static_cast<size_t>(resampler_.taps()), SampleType{0});
        }
        reset();
    }

    /// Fixed pipeline delay of process() in realtime_pitch mode.
    int latency_samples() const { return fft_size_ + analysis_hop_ + kReadGuard; }
    int fft_size() const { return fft_size_; }

    void set_pitch_semitones(float semitones) {
        pitch_smoother_.set_target(
            std::clamp(semitones, -config_.max_pitch_semitones, config_.max_pitch_semitones));
    }

    void set_formant_semitones(float semitones) {
        formant_smoother_.set_target(
            std::clamp(semitones, -config_.max_pitch_semitones, config_.max_pitch_semitones));
    }

    void set_formant_mode(FormantMode mode) { config_.formant_mode = mode; }

    /// Freeze (infinite hold) of the current input moment. Held audio
    /// remains pitch/formant-controllable: the hold replaces analysis
    /// frames at the head of the chain, upstream of phase propagation.
    void set_frozen(bool frozen) { freeze_.set_frozen(frozen); }
    bool is_frozen() const { return freeze_.is_latched(); }

    /// time_stretch mode only; > 1 lengthens. Takes effect at the next frame.
    void set_time_ratio(float ratio) {
        time_ratio_ = static_cast<SampleType>(
            std::clamp(ratio, 1.0f / config_.max_time_ratio, config_.max_time_ratio));
    }

    /// Hop-quantized stretch actually applied so far (time_stretch mode).
    double achieved_time_ratio() const {
        return frames_done_ > 0
                   ? static_cast<double>(synth_accum_int_)
                         / (static_cast<double>(frames_done_) * analysis_hop_)
                   : static_cast<double>(time_ratio_);
    }

    /// realtime_pitch mode: consume and produce exactly `num_samples`.
    void process(const SampleType* const* in, SampleType* const* out, int num_samples) {
        assert(config_.mode == PitchTimeMode::realtime_pitch);
        assert(num_samples <= config_.max_block);

        feed_engine(in, num_samples);
        pitch_smoother_.advance(num_samples);
        formant_smoother_.advance(num_samples);

        // Catmull-Rom read of the stretched stream, positioned through
        // the producer's own frame map: frame f covers input starting at
        // f * hop and was synthesized starting at stretched position
        // S_f, so the stretched position playing input time t is the
        // piecewise-linear interpolation through the (f * hop -> S_f)
        // pairs. Output sample T plays input time T - latency exactly —
        // no open-loop ratio integration, hence no drift against the hop
        // accumulator, exact latency at every ratio, and the local read
        // slope (the pitch ratio) is consistent with the synthesis hops
        // by construction. The reader trails the producer by under two
        // frames, which the latency guard plus drain cadence always
        // covers in realtime use.
        const auto lat = static_cast<std::int64_t>(latency_samples());
        const auto hop = static_cast<std::int64_t>(analysis_hop_);
        for (int i = 0; i < num_samples; ++i) {
            if (out_count_ < lat) {
                for (int ch = 0; ch < config_.channels; ++ch) out[ch][i] = SampleType{0};
            } else {
                const std::int64_t target_in = out_count_ - lat;
                const std::int64_t f = target_in / hop;
                if (f + 1 < frames_done_) {
                    const std::int64_t s0 = frame_starts_[static_cast<size_t>(f & kFrameMapMask)];
                    const std::int64_t s1 =
                        frame_starts_[static_cast<size_t>((f + 1) & kFrameMapMask)];
                    const double frac =
                        static_cast<double>(target_in - f * hop) / static_cast<double>(hop);
                    read_pos_ = static_cast<double>(s0)
                              + frac * static_cast<double>(s1 - s0);
                    read_fractional(out, i);
                } else {
                    for (int ch = 0; ch < config_.channels; ++ch) out[ch][i] = SampleType{0};
                }
            }
            ++out_count_;
        }
    }

    /// time_stretch mode: push input; stretched output accumulates.
    void feed(const SampleType* const* in, int num_samples) {
        assert(config_.mode == PitchTimeMode::time_stretch);
        feed_engine(in, num_samples);
        pitch_smoother_.advance(num_samples);
        formant_smoother_.advance(num_samples);
    }

    /// time_stretch mode: stretched samples ready to read.
    int available_stretched() const {
        return static_cast<int>(stretch_written_ - stretch_read_);
    }

    /// time_stretch mode: pop stretched samples (caller respects
    /// available_stretched(); excess is zero-filled without advancing).
    void read_stretched(SampleType* const* out, int num_samples) {
        for (int i = 0; i < num_samples; ++i) {
            const bool valid = stretch_read_ < stretch_written_;
            const auto idx = static_cast<size_t>(stretch_read_ & ring_mask_);
            for (int ch = 0; ch < config_.channels; ++ch)
                out[ch][i] = valid
                                 ? stretch_ring_[static_cast<size_t>(ch) * ring_size_ + idx]
                                 : SampleType{0};
            if (valid) ++stretch_read_;
        }
    }

    void reset() {
        engine_.reset();
        coordinator_.reset();
        transient_.reset();
        freeze_.reset();
        if (config_.noise_morphing) {
            stn_.reset();
            for (auto& m : noise_morphers_) m.reset();
        }
        std::fill(stretch_ring_.begin(), stretch_ring_.end(), SampleType{0});
        pitch_smoother_.set_immediate(pitch_smoother_.target());
        formant_smoother_.set_immediate(formant_smoother_.target());
        synth_accum_ = 0.0;
        synth_accum_int_ = 0;
        frames_done_ = 0;
        stretch_written_ = 0;
        stretch_read_ = 0;
        read_pos_ = 0.0;
        std::fill(std::begin(frame_starts_), std::end(frame_starts_),
                  static_cast<std::int64_t>(0));
        out_count_ = 0;
        input_count_ = 0;
    }

private:
    // Push input through analysis; per frame: phase-propagate, apply the
    // formant correction, synthesize at the accumulated hop, and drain
    // the engine's final samples into the stretched ring.
    void feed_engine(const SampleType* const* in, int num_samples) {
        int done = 0;
        while (done < num_samples) {
            // Chunk exactly to the next analysis-frame boundary so offset_in_block_
            // is the true in-block offset at which the frame completes. handle_frame
            // evaluates the smoothed pitch/formant ratio at offset_in_block_, so a
            // boundary-aligned chunk makes that per-frame control offset exact.
            const int until = std::max(1, engine_.samples_until_next_frame());
            const int run = std::min(num_samples - done, until);
            offset_in_block_ = done + run;
            engine_.analyze(advance_ptrs(in, done), run,
                            [this](std::complex<SampleType>* const* frames, int bins) {
                                handle_frame(frames, bins);
                            });
            done += run;
            input_count_ += run;
        }
    }

    void handle_frame(std::complex<SampleType>* const* frames, int bins) {
        // Controls evaluated at the frame boundary. The smoothers still
        // sit at the block start here (they advance after feeding), so
        // the frame's in-block position is a positive forward offset.
        const SampleType pitch_ratio =
            config_.mode == PitchTimeMode::realtime_pitch
                ? static_cast<SampleType>(pitch_smoother_.ratio_at(offset_in_block_))
                : SampleType{1};
        const SampleType formant_ratio =
            static_cast<SampleType>(formant_smoother_.ratio_at(offset_in_block_));
        const SampleType stretch = config_.mode == PitchTimeMode::time_stretch
                                  ? time_ratio_
                                  : pitch_ratio;

        // Integer synthesis hop from the fractional accumulator, so the
        // average hop tracks stretch * analysis_hop exactly.
        synth_accum_ += static_cast<double>(stretch) * analysis_hop_;
        int hop = static_cast<int>(std::llround(synth_accum_) - synth_accum_int_);
        hop = std::clamp(hop, 1, static_cast<int>(std::ceil(
                                     static_cast<double>(stretch) * analysis_hop_))
                                 + 1);
        // Record this frame's stretched start position for the reader's
        // input-time -> stretched-position map.
        frame_starts_[static_cast<size_t>(frames_done_ & kFrameMapMask)] = synth_accum_int_;
        synth_accum_int_ += hop;
        ++frames_done_;

        // Freeze first: held frames look like live steady-state input, so
        // pitch/formant control downstream keeps working over the hold.
        freeze_.process_group(frames, config_.channels, bins);

        // Morphing only helps when time/pitch scaling is actually engaged;
        // at unity the baseline path is exactly transparent, so bypass the
        // split (which would otherwise re-randomise the phase of low-energy
        // bins and shallow the null).
        const bool morph =
            config_.noise_morphing
            && std::abs(static_cast<double>(stretch - SampleType{1})) > 1e-4;

        // Noise-morphing split: pull the noise component out of the vocoder
        // path so phase propagation only sees the sines+transients (which it
        // handles well); the noise is regenerated separately and added back
        // after the tonal processing (so the coordinator never re-locks its
        // random phase). The STN mask is computed from channel 0 and shared,
        // giving a coherent split; per-channel morphers decorrelate the
        // stereo noise. The mask carries the StnDecomposer's small inherent
        // delay, applied to the current frame — inaudible for stationary
        // noise.
        if (morph) {
            for (int k = 0; k < bins; ++k)
                mag_scratch_[static_cast<size_t>(k)] = std::abs(frames[0][k]);
            const StnMasksT<SampleType>& masks = stn_.process(mag_scratch_.data());
            for (int ch = 0; ch < config_.channels; ++ch) {
                std::complex<SampleType>* f = frames[ch];
                SampleType* env = noise_env_.data() + static_cast<size_t>(ch) * bins;
                for (int k = 0; k < bins; ++k) {
                    const SampleType nm = masks.noise[static_cast<size_t>(k)];
                    env[k] = std::abs(f[k]) * nm;
                    f[k] *= (SampleType{1} - nm);
                }
            }
        }

        const SampleType reset_amount =
            config_.transient_preservation
                ? transient_.analyze(frames, config_.channels, bins)
                : SampleType{0};
        coordinator_.process_group(frames, bins, analysis_hop_, hop, reset_amount);

        // Formant path: warp = pitch_ratio / formant_ratio in preserve
        // mode (cancels the resampler's envelope scaling), 1 / formant_ratio
        // in follow mode. warp == 1 is an exact bypass inside the shifter.
        const SampleType warp =
            (config_.formant_mode == FormantMode::preserve ? pitch_ratio : SampleType{1})
            / formant_ratio;
        envelope_.process_group(frames, config_.channels, bins, warp);

        // Add the morphed noise back to the (now phase-propagated) tonal
        // frame: each channel regenerates its noise from the captured
        // envelope with fresh random phase, so successive synthesis frames
        // are decorrelated and overlap-add to natural noise of the right
        // color at any stretch ratio.
        if (morph) {
            // Random-phase noise frames overlap-add INCOHERENTLY while the WOLA
            // normalizes for COHERENT summation, so morphed noise renders ~4-5 dB
            // too quiet (an energy-conservation bug: measured -4 to -8 LUFS total
            // signal). Compensate by the Hann random-phase factor sqrt(8/3) ≈ 1.63
            // (Hann mean-square), with a gentle overlap correction since denser
            // overlap (time compression) loses a little more.
            const SampleType overlap = static_cast<SampleType>(fft_size_)
                                     / static_cast<SampleType>(hop);
            const SampleType noise_gain = std::sqrt(static_cast<SampleType>(8.0 / 3.0))
                                        * std::pow(overlap / static_cast<SampleType>(8.0),
                                                   static_cast<SampleType>(0.08));
            for (int ch = 0; ch < config_.channels; ++ch) {
                std::complex<SampleType>* f = frames[ch];
                SampleType* env = noise_env_.data() + static_cast<size_t>(ch) * bins;
                NoiseMorpherT<SampleType>& m = noise_morphers_[static_cast<size_t>(ch)];
                m.set_synthesis_gain(noise_gain);
                m.push_envelope(env);
                m.synthesize(SampleType{1}, noise_spec_.data());
                for (int k = 0; k < bins; ++k) f[k] += noise_spec_[static_cast<size_t>(k)];
            }
        }

        engine_.synthesize_frame(frames, hop);
        drain_engine();
    }

    void drain_engine() {
        int avail = engine_.available_output();
        while (avail > 0) {
            const int chunk = std::min(avail, static_cast<int>(
                drain_buf_.size() / static_cast<size_t>(config_.channels)));
            for (int ch = 0; ch < config_.channels; ++ch)
                drain_ptrs_[static_cast<size_t>(ch)] =
                    drain_buf_.data() + static_cast<size_t>(ch)
                    * (drain_buf_.size() / static_cast<size_t>(config_.channels));
            engine_.read_output(drain_ptrs_.data(), chunk);
            for (int ch = 0; ch < config_.channels; ++ch) {
                const SampleType* src = drain_ptrs_[static_cast<size_t>(ch)];
                SampleType* ring = stretch_ring_.data() + static_cast<size_t>(ch) * ring_size_;
                for (int i = 0; i < chunk; ++i)
                    ring[static_cast<size_t>((stretch_written_ + i) & ring_mask_)] = src[i];
            }
            stretch_written_ += chunk;
            avail -= chunk;
        }
    }

    // Fractional read of the stretched ring at read_pos_ — Catmull-Rom
    // cubic by default, or a Kaiser-windowed sinc kernel when enabled (lower
    // aliasing on the pitch-shift resample step).
    void read_fractional(SampleType* const* out, int i) {
        if (stretch_written_ < 4) {
            for (int ch = 0; ch < config_.channels; ++ch) out[ch][i] = SampleType{0};
            return;
        }
        const auto i1 = static_cast<std::int64_t>(read_pos_);
        const SampleType t = static_cast<SampleType>(read_pos_ - static_cast<double>(i1));
        const std::int64_t last = stretch_written_ - 1;
        const auto clamp_idx = [&](std::int64_t p) {
            return static_cast<size_t>(std::clamp<std::int64_t>(p, 0, last) & ring_mask_);
        };
        if (config_.sinc_resampling) {
            const int taps = resampler_.taps();
            const int half = resampler_.half_width();
            for (int ch = 0; ch < config_.channels; ++ch) {
                const SampleType* ring = stretch_ring_.data()
                                       + static_cast<size_t>(ch) * ring_size_;
                // Gather the kernel neighbourhood (i1-half+1 .. i1+half) from
                // the ring with edge clamping, then apply the sinc kernel.
                for (int k = 0; k < taps; ++k)
                    tap_scratch_[static_cast<size_t>(k)] =
                        ring[clamp_idx(i1 + k - half + 1)];
                out[ch][i] = resampler_.apply(tap_scratch_.data(), t);
            }
            return;
        }
        for (int ch = 0; ch < config_.channels; ++ch) {
            const SampleType* ring = stretch_ring_.data()
                                   + static_cast<size_t>(ch) * ring_size_;
            const SampleType p0 = ring[clamp_idx(i1 - 1)];
            const SampleType p1 = ring[clamp_idx(i1)];
            const SampleType p2 = ring[clamp_idx(i1 + 1)];
            const SampleType p3 = ring[clamp_idx(i1 + 2)];
            const SampleType a = static_cast<SampleType>(0.5)
                               * (-p0 + static_cast<SampleType>(3) * p1
                                  - static_cast<SampleType>(3) * p2 + p3);
            const SampleType b = p0 - static_cast<SampleType>(2.5) * p1
                               + static_cast<SampleType>(2) * p2
                               - static_cast<SampleType>(0.5) * p3;
            const SampleType c = static_cast<SampleType>(0.5) * (p2 - p0);
            out[ch][i] = ((a * t + b) * t + c) * t + p1;
        }
    }

    const SampleType* const* advance_ptrs(const SampleType* const* in, int offset) {
        for (int ch = 0; ch < config_.channels; ++ch)
            in_ptrs_scratch_[ch] = in[ch] + offset;
        return in_ptrs_scratch_;
    }

    static constexpr int kReadGuard = 8;   // frame-map + cubic-interp headroom
    static constexpr int kFrameMapMask = 63;

    RealtimePitchTimeConfig config_;
    double sample_rate_ = 48000.0;
    int fft_size_ = 4096;
    int analysis_hop_ = 512;
    SampleType time_ratio_ = SampleType{1};

    SpectralFrameEngineT<SampleType> engine_;
    MultichannelPhaseCoordinatorT<SampleType> coordinator_;
    SpectralEnvelopeShifterT<SampleType> envelope_;
    TransientPhasePolicyT<SampleType> transient_;
    FreezeHoldT<SampleType> freeze_;
    LatencyAwareControlSmoother pitch_smoother_;
    LatencyAwareControlSmoother formant_smoother_;

    // Noise-morphing front end (allocated only when config_.noise_morphing).
    StnDecomposerT<SampleType> stn_;
    std::vector<NoiseMorpherT<SampleType>> noise_morphers_;
    std::vector<SampleType> mag_scratch_;
    std::vector<SampleType> noise_env_; // channels * spectral_bins
    std::vector<std::complex<SampleType>> noise_spec_;

    // Sinc resampler (allocated only when config_.sinc_resampling).
    SincResamplerT<SampleType> resampler_;
    std::vector<SampleType> tap_scratch_; // resampler_.taps() gather buffer

    int ring_size_ = 0;
    int ring_mask_ = 0;
    std::vector<SampleType> stretch_ring_;
    std::vector<SampleType> drain_buf_;
    std::vector<SampleType*> drain_ptrs_;
    static constexpr int kMaxChannels = 64;
    const SampleType* in_ptrs_scratch_[kMaxChannels] = {};

    double synth_accum_ = 0.0;
    std::int64_t synth_accum_int_ = 0;
    std::int64_t frames_done_ = 0;
    std::int64_t stretch_written_ = 0;
    std::int64_t stretch_read_ = 0;
    double read_pos_ = 0.0;
    std::int64_t frame_starts_[kFrameMapMask + 1] = {};
    std::int64_t out_count_ = 0;
    std::int64_t input_count_ = 0;
    int offset_in_block_ = 0;
};

using RealtimePitchTimeProcessor = RealtimePitchTimeProcessorT<float>;
using RealtimePitchTimeProcessor64 = RealtimePitchTimeProcessorT<double>;

} // namespace pulp::signal
