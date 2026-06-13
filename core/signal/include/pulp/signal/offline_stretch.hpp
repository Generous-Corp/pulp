#pragma once
/// @file offline_stretch.hpp
/// Offline (non-realtime), maximum-quality time-stretch / pitch-shift / formant
/// engine. It orchestrates the realtime spectral primitives already in
/// pulp::signal — SpectralFrameEngine (STFT/WOLA), RealtimePitchTimeProcessor
/// (Laroche-Dolson TSM+resample, peak phase locking), SpectralEnvelopeShifter
/// (formant), TransientPhasePolicy (transient reset), StnDecomposer/NoiseMorpher
/// (natural noise stretch) and Resampler (polyphase Kaiser-sinc) — over the
/// WHOLE input with no latency constraint, and adds the offline-only
/// refinements a realtime design cannot do: a distributed time-warp length-lock
/// to an EXACT output length, non-causal (look-ahead) transient handling, and
/// (gated) verbatim transient relocation.
///
/// Design + task plan: planning/Sampler-Offline-Stretch-Build-Plan.md.
///
/// Tiering: this orchestration is the v1 BASELINE. The realtime core is
/// hop-quantized and causal; if measured quality fails the Rubber Band R3 bar
/// (see the metrics harness), the plan opens a native offline path. The
/// realtime core's validated numbers are realtime results — offline re-proves.
///
/// ─────────────────────────────────────────────────────────────────────────
/// PHASE 0 SCAFFOLD: this header defines the stable public API and compiles as
/// an explicit, length-correct pass-through (exact at time_ratio == 1, pitch 0).
/// The spectral orchestration and the offline refinements land in Phase 1+.
/// Each unimplemented path is marked `// TODO(phaseN)`.
/// ─────────────────────────────────────────────────────────────────────────

#include <pulp/signal/interpolator.hpp>

#include <cmath>
#include <string>

namespace pulp::signal {

/// Formant behaviour for the spectral modes. signal::FormantMode on main is
/// only {follow, preserve} and cannot express the three-way behaviour we need,
/// so this is an offline-specific enum (plan §5). Each value maps to a concrete
/// SpectralEnvelopeShifter `warp` at wiring time (the shifter exposes a single
/// warp knob, not a mode enum):
///   follow_pitch        -> warp = 1                       (formants ride the shift)
///   preserve_original   -> warp = pitch_ratio             (formants held at source)
///   shift_independently -> warp = pitch_ratio/formant_ratio (decoupled via
///                          `formant_semitones`)
enum class OfflineFormantMode { follow_pitch, preserve_original, shift_independently };

/// Transient strategy. `phase_reset` uses the (causal) TransientPhasePolicy
/// Röbel reset; `verbatim_relocate` is the offline-only copy-through path,
/// gated on the seam-quality + blind-A/B criteria in the plan (§6).
enum class StretchTransientMode { phase_reset, verbatim_relocate };

/// Whole-input render options. See plan §5.
struct OfflineStretchOptions {
    double time_ratio = 1.0;        ///< output duration / input duration
    double pitch_semitones = 0.0;   ///< fractional allowed
    OfflineFormantMode formant_mode = OfflineFormantMode::preserve_original;
    double formant_semitones = 0.0; ///< used only when formant_mode==shift_independently
    bool repitch_linked = false;    ///< true => pure resample (vinyl); pitch
                                    ///< follows time_ratio, spectral path skipped
    bool route_noise_stn = true;    ///< route noise/residual through NoiseMorpher
    StretchTransientMode transient_mode = StretchTransientMode::phase_reset;
    int quality = 2;                ///< 0 draft (fast preview) .. 2 best

    // Range MUST be sized up-front (plan §3.6 / §5): the underlying
    // RealtimePitchTimeProcessor clamps to [1/max, max] and allocates from these
    // bounds at prepare(). The sampler's tempo ratios (host_bpm/loop_bpm)
    // routinely exceed 0.5–2×, so the defaults are wider than the realtime
    // engine's. A process() ratio beyond the PREPARED bounds is REJECTED, never
    // silently clamped — so a mis-sized render is a loud error, not a wrong result.
    double max_time_ratio = 4.0;      ///< supported stretch span is [1/max, max]
    double max_pitch_semitones = 24.0;
};

/// The exact, sample-accurate output length for an input of `in_frames` frames
/// at the given time ratio: round(in_frames * time_ratio). The sampler relies
/// on this being exact so stretched loops stay locked to the host bar grid.
/// This is the single source of truth for the output length and MUST match the
/// number of frames `process()` writes.
inline long offline_stretch_output_frames(long in_frames, double time_ratio) noexcept {
    if (in_frames <= 0 || !(time_ratio > 0.0)) return 0;
    const double exact = static_cast<double>(in_frames) * time_ratio;
    const long out = static_cast<long>(std::lround(exact));
    return out < 0 ? 0 : out;
}

/// Offline stretcher. One instance per concurrent render; not thread-safe for
/// concurrent calls on the SAME instance, but distinct instances are
/// independent (the sampler renders slices on multiple background threads).
class OfflineStretch {
public:
    /// Size internal state for `channels` at `sample_rate`. Must be called
    /// before process(). Allocation happens here, not in process(). `sizing`
    /// fixes the supported range: process() ratios must stay within
    /// [1/max_time_ratio, max_time_ratio] and |pitch| within max_pitch_semitones
    /// (plan §3.6). Defaults give a [0.25×, 4×] / ±24 st envelope; pass wider
    /// bounds before rendering extreme tempo matches.
    void prepare(double sample_rate, int channels,
                 const OfflineStretchOptions& sizing = {}) {
        sample_rate_ = sample_rate;
        channels_ = channels < 1 ? 1 : channels;
        max_time_ratio_ = sizing.max_time_ratio >= 1.0 ? sizing.max_time_ratio : 1.0;
        max_pitch_semitones_ = sizing.max_pitch_semitones >= 0.0
                                   ? sizing.max_pitch_semitones : 0.0;
        prepared_ = (sample_rate > 0.0) && (channels >= 1);
        // TODO(phase1): size SpectralFrameEngine / RealtimePitchTimeProcessor /
        // SpectralEnvelopeShifter / Resampler scratch for the prepared geometry
        // and the prepared max ranges above.
    }

    int channels() const noexcept { return channels_; }
    double sample_rate() const noexcept { return sample_rate_; }
    double max_time_ratio() const noexcept { return max_time_ratio_; }
    double max_pitch_semitones() const noexcept { return max_pitch_semitones_; }

    /// Render the whole input into the caller-allocated output. `out_frames`
    /// MUST equal offline_stretch_output_frames(in_frames, opts.time_ratio).
    /// Deinterleaved float32; `in`/`out` are arrays of `channels()` pointers.
    /// Returns false (with *err set, if provided) on a contract violation.
    bool process(const float* const* in, long in_frames,
                 float* const* out, long out_frames,
                 const OfflineStretchOptions& opts,
                 std::string* err = nullptr) {
        if (!prepared_)            return fail(err, "OfflineStretch::process called before prepare()");
        if (in_frames < 0)         return fail(err, "in_frames must be >= 0");
        if (out_frames < 0)        return fail(err, "out_frames must be >= 0");
        if (in == nullptr && in_frames > 0)   return fail(err, "null input");
        if (out == nullptr && out_frames > 0) return fail(err, "null output");
        if (!(opts.time_ratio > 0.0))         return fail(err, "time_ratio must be > 0");

        // Range is fixed at prepare() — reject, never silently clamp (plan §3.6).
        if (opts.time_ratio > max_time_ratio_ || opts.time_ratio < 1.0 / max_time_ratio_)
            return fail(err, "time_ratio outside the prepared range [1/max, max]; "
                             "widen OfflineStretchOptions::max_time_ratio at prepare()");
        if (std::abs(opts.pitch_semitones) > max_pitch_semitones_)
            return fail(err, "pitch_semitones outside the prepared range; widen "
                             "OfflineStretchOptions::max_pitch_semitones at prepare()");

        const long expected = offline_stretch_output_frames(in_frames, opts.time_ratio);
        if (out_frames != expected)
            return fail(err, "out_frames must equal round(in_frames * time_ratio)");

        // Linked / vinyl mode: pure high-quality resample. Output sample i reads
        // input position i/ratio at a constant rate, so pitch tracks tempo
        // exactly (factor 1/ratio) and the output is exactly `expected` frames by
        // construction. sinc6 (6-point Blackman-Harris windowed sinc) is
        // mastering-grade and is an exact identity at ratio == 1.
        if (opts.repitch_linked) {
            for (int c = 0; c < channels_; ++c)
                for (long i = 0; i < out_frames; ++i)
                    out[c][i] = sample_sinc6(in[c], in_frames,
                                             static_cast<double>(i) / opts.time_ratio);
            return true;
        }

        // TODO(phase1): tempo-only spectral path -> RealtimePitchTimeProcessor
        //               whole-file + distributed time-warp length-lock.
        // TODO(phase2): pitch / formant / single-pass R+S / STN routing.
        // TODO(phase3): non-causal transient handling, verbatim relocation.
        //
        // Until the spectral path lands, the non-repitch path is a length-correct
        // pass-through: exact identity at ratio == 1 (null test), else copy the
        // overlapping span and zero-pad — an honest placeholder, NOT a stretch.
        for (int c = 0; c < channels_; ++c) {
            float* dst = out[c];
            const float* src = in[c];
            const long n = (in_frames < out_frames) ? in_frames : out_frames;
            for (long i = 0; i < n; ++i) dst[i] = src[i];
            for (long i = n; i < out_frames; ++i) dst[i] = 0.0f;
        }
        return true;
    }

private:
    // 6-point Blackman-Harris windowed-sinc read of `x` at fractional position
    // `pos`; out-of-range taps read as silence (edge zero-pad). Exact identity
    // when pos is integral.
    static float sample_sinc6(const float* x, long n, double pos) {
        const long i0 = static_cast<long>(std::floor(pos));
        const float frac = static_cast<float>(pos - static_cast<double>(i0));
        auto at = [&](long k) -> float { return (k >= 0 && k < n) ? x[k] : 0.0f; };
        return Interpolator::sinc6(frac, at(i0 - 2), at(i0 - 1), at(i0),
                                   at(i0 + 1), at(i0 + 2), at(i0 + 3));
    }

    static bool fail(std::string* err, const char* msg) {
        if (err) *err = msg;
        return false;
    }

    double sample_rate_ = 0.0;
    int channels_ = 1;
    double max_time_ratio_ = 4.0;
    double max_pitch_semitones_ = 24.0;
    bool prepared_ = false;
};

} // namespace pulp::signal
