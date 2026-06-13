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

#include <cmath>
#include <string>

namespace pulp::signal {

/// Formant behaviour for the spectral modes (maps to SpectralEnvelopeShifter
/// warp at wiring time). `independent` decouples the formant scale from the
/// pitch shift via `formant_semitones`.
enum class OfflineFormantMode { shifted, preserved, independent };

/// Transient strategy. `phase_reset` uses the (causal) TransientPhasePolicy
/// Röbel reset; `verbatim_relocate` is the offline-only copy-through path,
/// gated on the seam-quality + blind-A/B criteria in the plan (§6).
enum class StretchTransientMode { phase_reset, verbatim_relocate };

/// Whole-input render options. See plan §5.
struct OfflineStretchOptions {
    double time_ratio = 1.0;        ///< output duration / input duration
    double pitch_semitones = 0.0;   ///< fractional allowed
    OfflineFormantMode formant_mode = OfflineFormantMode::preserved;
    double formant_semitones = 0.0; ///< used only when formant_mode==independent
    bool repitch_linked = false;    ///< true => pure resample (vinyl); pitch
                                    ///< follows time_ratio, spectral path skipped
    bool route_noise_stn = true;    ///< route noise/residual through NoiseMorpher
    StretchTransientMode transient_mode = StretchTransientMode::phase_reset;
    int quality = 2;                ///< 0 draft (fast preview) .. 2 best
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
    /// before process(). Allocation happens here, not in process().
    void prepare(double sample_rate, int channels) {
        sample_rate_ = sample_rate;
        channels_ = channels < 1 ? 1 : channels;
        prepared_ = (sample_rate > 0.0) && (channels >= 1);
        // TODO(phase1): size SpectralFrameEngine / RealtimePitchTimeProcessor /
        // SpectralEnvelopeShifter / Resampler scratch for the prepared geometry.
    }

    int channels() const noexcept { return channels_; }
    double sample_rate() const noexcept { return sample_rate_; }

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

        const long expected = offline_stretch_output_frames(in_frames, opts.time_ratio);
        if (out_frames != expected)
            return fail(err, "out_frames must equal round(in_frames * time_ratio)");

        // TODO(phase1): repitch_linked -> Resampler.
        // TODO(phase1): spectral path -> RealtimePitchTimeProcessor whole-file +
        //               distributed time-warp length-lock to `expected`.
        // TODO(phase2): pitch / formant / single-pass R+S / STN routing.
        // TODO(phase3): non-causal transient handling, verbatim relocation.
        //
        // PHASE 0: length-correct pass-through. Exact identity at time_ratio==1
        // (null test passes); otherwise copy the overlapping span and zero-pad,
        // which is an honest placeholder — NOT yet a stretch.
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
    static bool fail(std::string* err, const char* msg) {
        if (err) *err = msg;
        return false;
    }

    double sample_rate_ = 0.0;
    int channels_ = 1;
    bool prepared_ = false;
};

} // namespace pulp::signal
