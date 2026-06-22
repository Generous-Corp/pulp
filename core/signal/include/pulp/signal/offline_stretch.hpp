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
/// Tiering: this orchestration is the v1 BASELINE. The realtime core is
/// hop-quantized and causal; if measured quality fails the Rubber Band R3 bar
/// (see the metrics harness), this API can grow a native offline path. The
/// realtime core's validated numbers are realtime results; offline must prove
/// its own quality.
///
/// ─────────────────────────────────────────────────────────────────────────
/// Current baseline: tempo stretch, pitch shift, varispeed, and linked resample
/// routes are implemented over the realtime spectral primitives. The identity
/// route remains exact at `time_ratio == 1` and `pitch_semitones == 0`; offline
/// refinements such as seam-clean transient relocation remain gated.
/// ─────────────────────────────────────────────────────────────────────────

#include <pulp/signal/interpolator.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pulp::signal {

/// Formant behaviour for the spectral modes. signal::FormantMode on main is
/// only {follow, preserve} and cannot express the three-way behaviour we need,
/// so this is an offline-specific enum. Each value maps to a concrete
/// SpectralEnvelopeShifter `warp` at wiring time (the shifter exposes a single
/// warp knob, not a mode enum):
///   follow_pitch        -> warp = 1                       (formants ride the shift)
///   preserve_original   -> warp = pitch_ratio             (formants held at source)
///   shift_independently -> warp = pitch_ratio/formant_ratio (decoupled via
///                          `formant_semitones`)
enum class OfflineFormantMode { follow_pitch, preserve_original, shift_independently };

/// Transient strategy. `phase_reset` uses the (causal) TransientPhasePolicy
/// Röbel reset; `verbatim_relocate` is the offline-only copy-through path,
/// gated on seam-quality and blind-A/B acceptance criteria.
enum class StretchTransientMode { phase_reset, verbatim_relocate };

/// Character mode — the musically-distinct "engine per job" selector. Each has a
/// different artifact signature; pick by material/taste.
///   clean         — peak-lock phase vocoder + material-adaptive FFT (the default).
///                   Natural, time != pitch. Best for tonal/melodic/sustained.
///   varispeed     — pitch+time LINKED (pure resample) + speed-scaled tape head EQ.
///                   Tape character, NO stretch artifacts. Pitch follows tempo.
///   phase_vocoder — reserved for clean + verbatim transient relocation; currently
///                   renders as `clean` until seam handling passes the quality gate.
///   granular      — reserved for grain/stutter texture; currently renders as `clean`.
enum class StretchCharacter { clean, varispeed, phase_vocoder, granular };

/// Whole-input render options.
struct OfflineStretchOptions {
    /// Character mode (see StretchCharacter). Default `clean`. `varispeed` ignores
    /// pitch_semitones (pitch is linked to time_ratio); the others honor it.
    StretchCharacter character = StretchCharacter::clean;
    /// Cross-engine: graft verbatim transients onto the stretched output for punch.
    /// Current limitation: seam-clean relocation is not enabled yet, so this is a no-op.
    bool relocate_transients = false;
    double time_ratio = 1.0;        ///< output duration / input duration
    double pitch_semitones = 0.0;   ///< fractional allowed
    OfflineFormantMode formant_mode = OfflineFormantMode::preserve_original;
    double formant_semitones = 0.0; ///< used only when formant_mode==shift_independently
    bool repitch_linked = false;    ///< true => pure resample (vinyl); pitch
                                    ///< follows time_ratio, spectral path skipped
    // STN noise-morphing OFF by default. Measured (Python-vs-C++ A/B across
    // bass/mix/vocal/drum at 0.75–3×): routing the residual through NoiseMorpher
    // DULLS the output ~400 spectral-centroid points on every material — the muddy
    // drum + the "wind"/modulation the validated Python prototype never had (it has
    // no STN). Off restores the Python brightness (drum 2521→2953 ≈ source 2990).
    // Opt back in per-render for genuinely noise-dominated textures where natural
    // residual stretch matters more than HF brightness. (The morpher's HF loss is
    // a separate bug to fix before re-enabling by default.)
    bool route_noise_stn = false;   ///< route noise/residual through NoiseMorpher
    StretchTransientMode transient_mode = StretchTransientMode::phase_reset;
    int quality = 2;                ///< 0 draft (fast preview) .. 2 best

    // Range MUST be sized up-front: the underlying
    // RealtimePitchTimeProcessor clamps to [1/max, max] and allocates from these
    // bounds at prepare(). The sampler's tempo ratios (host_bpm/loop_bpm)
    // routinely exceed 0.5–2×, so the defaults are wider than the realtime
    // engine's. A process() ratio beyond the PREPARED bounds is REJECTED, never
    // silently clamped — so a mis-sized render is a loud error, not a wrong result.
    double max_time_ratio = 4.0;      ///< supported stretch span is [1/max, max]
    double max_pitch_semitones = 24.0;

    // Material-adaptive STFT geometry (ear-validated). The phase core
    // is fixed at FFT 4096/512; that window is too SMALL to resolve closely-
    // spaced low partials (bass stretches wobble) and too LARGE for percussive
    // time resolution (drum attacks soften). Setting these picks a window suited
    // to the input — large+high-overlap for bass, small for transients. 0 = keep
    // the engine default. Use OfflineStretch::recommend_window() to choose them
    // from the actual audio, then pass here at prepare().
    int fft_size = 0;       ///< 0 = default (4096); else power-of-two ≥ 256
    int analysis_hop = 0;   ///< 0 = default (512); must divide fft_size

    // Transient-detector sensitivity (0 = engine default 1.0). Higher keeps EVERY
    // percussion hit sharp; the default detector softens some hits ("transients
    // move around"). recommend_window pairs a higher value with the percussive
    // window; a caller can override.
    float transient_sensitivity = 0.0f;
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
    /// at prepare(). Defaults give a [0.25×, 4×] / ±24 st envelope; pass wider
    /// bounds before rendering extreme tempo matches.
    void prepare(double sample_rate, int channels,
                 const OfflineStretchOptions& sizing = {}) {
        sample_rate_ = sample_rate;
        channels_ = channels < 1 ? 1 : channels;
        max_time_ratio_ = sizing.max_time_ratio >= 1.0 ? sizing.max_time_ratio : 1.0;
        max_pitch_semitones_ = sizing.max_pitch_semitones >= 0.0
                                   ? sizing.max_pitch_semitones : 0.0;
        prepared_ = (sample_rate > 0.0) && (channels >= 1);
        if (prepared_) {
            // Size the tempo-only engine FROM the prepared bound — not the
            // engine's default 2.0x — so host_bpm/loop_bpm ratios up to
            // max_time_ratio_ render without silent clamping.
            RealtimePitchTimeConfig cfg;
            cfg.mode = PitchTimeMode::time_stretch;
            cfg.quality = PitchTimeQuality::quality;
            cfg.channels = channels_;
            cfg.max_block = kBlock;
            // Size for the R*P product so the single-pass independent path
            // (stretch by R*P, then resample by P) stays within engine bounds.
            const double max_pitch_ratio =
                std::exp2((max_pitch_semitones_ > 0.0 ? max_pitch_semitones_ : 0.0) / 12.0);
            cfg.max_time_ratio = static_cast<float>(max_time_ratio_ * max_pitch_ratio);
            cfg.max_pitch_semitones = 0.0f; // pitch is fixed in time_stretch mode
            cfg.transient_preservation = true;
            cfg.transient_sensitivity = sizing.transient_sensitivity; // keep every hit sharp
            cfg.noise_morphing = sizing.route_noise_stn; // STN noise path
            // Material-adaptive window/overlap (ear-validated: fixes bass wobble
            // and keeps drum transients natural; the realtime default is one size
            // too small for bass and too large for percussion).
            cfg.fft_size = sizing.fft_size;
            cfg.analysis_hop = sizing.analysis_hop;
            fft_size_ = sizing.fft_size;       // remembered for accessor/tests
            engine_.prepare(sample_rate, cfg);
            latency_anchor_ = calibrate_anchor();

            // Separate engine in realtime_pitch mode for the pitch path (formant
            // preserve/shift/independent live here, in the SpectralEnvelopeShifter).
            RealtimePitchTimeConfig pcfg;
            pcfg.mode = PitchTimeMode::realtime_pitch;
            pcfg.quality = PitchTimeQuality::quality;
            pcfg.channels = channels_;
            pcfg.max_block = kBlock;
            pcfg.max_pitch_semitones =
                static_cast<float>(max_pitch_semitones_ > 0.0 ? max_pitch_semitones_ : 1.0);
            pcfg.formant_mode = FormantMode::follow;
            pcfg.true_envelope_iterations = 3;
            pcfg.transient_preservation = true;
            pcfg.noise_morphing = sizing.route_noise_stn;
            pitch_engine_.prepare(sample_rate, pcfg);
        }
    }

    int channels() const noexcept { return channels_; }
    double sample_rate() const noexcept { return sample_rate_; }
    double max_time_ratio() const noexcept { return max_time_ratio_; }
    double max_pitch_semitones() const noexcept { return max_pitch_semitones_; }
    /// The STFT window actually in use (0 until prepare() with an override).
    int fft_size() const noexcept { return fft_size_; }

    /// Material-adaptive window/overlap recommendation. Analyzes the input and
    /// returns the {fft_size, analysis_hop} to set on OfflineStretchOptions
    /// before prepare(). Ear-validated geometry:
    ///   percussive (high crest)        -> 1024 / 128  (time resolution: sharp,
    ///                                     natural attacks)
    ///   bass / low-fundamental-heavy   -> 8192 / 512  (16× overlap; resolves
    ///                                     closely-spaced low partials so the
    ///                                     stretch doesn't wobble)
    ///   everything else                -> {0, 0}      (engine default 4096/512)
    /// Order matters: a kick-heavy break is BOTH low and percussive — transients
    /// win, or the long window smears every hit. `lo_band` is the energy fraction
    /// below 200 Hz; `crest` is peak/mean of a short-window energy envelope.
    struct Window { int fft_size; int analysis_hop; };
    static Window recommend_window(const float* const* in, long frames,
                                   int channels, double sample_rate) {
        if (in == nullptr || frames <= 0 || channels < 1 || !(sample_rate > 0.0))
            return {0, 0};
        // Mono mix.
        std::vector<double> x(static_cast<size_t>(frames), 0.0);
        for (int c = 0; c < channels; ++c) {
            const float* s = in[c];
            if (!s) continue;
            for (long i = 0; i < frames; ++i) x[static_cast<size_t>(i)] += s[i];
        }
        const double inv = 1.0 / channels;
        for (auto& v : x) v *= inv;

        // Crest of a 512-sample energy envelope (256 hop) = transient density.
        const long W = 512, H = 256;
        double e_sum = 0.0, e_max = 0.0; long e_cnt = 0;
        for (long i = 0; i + W <= frames; i += H) {
            double acc = 0.0;
            for (long j = 0; j < W; ++j) { const double v = x[static_cast<size_t>(i + j)]; acc += v * v; }
            const double rms = std::sqrt(acc / static_cast<double>(W) + 1e-12);
            e_sum += rms; e_max = std::max(e_max, rms); ++e_cnt;
        }
        const double crest = e_cnt > 0 ? e_max / (e_sum / static_cast<double>(e_cnt) + 1e-12) : 0.0;

        // Low-band fraction: RBJ 2nd-order lowpass @200 Hz, lowpassed energy /
        // total energy. Avoids an FFT dependency in the header.
        const double f0 = 200.0, q = 0.7071;
        const double w0 = 2.0 * 3.14159265358979323846 * f0 / sample_rate;
        const double cw = std::cos(w0), sw = std::sin(w0), alpha = sw / (2.0 * q);
        const double b0 = (1.0 - cw) * 0.5, b1 = 1.0 - cw, b2 = (1.0 - cw) * 0.5;
        const double a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;
        const double nb0 = b0 / a0, nb1 = b1 / a0, nb2 = b2 / a0, na1 = a1 / a0, na2 = a2 / a0;
        double z1 = 0.0, z2 = 0.0, lo_e = 0.0, tot_e = 0.0;
        for (long i = 0; i < frames; ++i) {
            const double in_s = x[static_cast<size_t>(i)];
            const double out_s = nb0 * in_s + z1;
            z1 = nb1 * in_s - na1 * out_s + z2;
            z2 = nb2 * in_s - na2 * out_s;
            lo_e += out_s * out_s; tot_e += in_s * in_s;
        }
        const double lo_band = tot_e > 0.0 ? lo_e / tot_e : 0.0;

        // Percussive: 1024/128 — matches the validated Python "drum_pl" reference
        // the user picked. (The earlier "wobble" that motivated a 2048 window was
        // the STN noise path, not the window; with STN off, 1024 gives the sharp,
        // bright drum attacks of the Python prototype.)
        if (crest > 3.0)    return {1024, 128};  // percussive — time resolution, match Python ref
        if (lo_band > 0.30) return {8192, 512};  // bass/low — resolve low partials
        return {0, 0};                           // default 4096/512
    }

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

        // Range is fixed at prepare() — reject, never silently clamp.
        if (opts.time_ratio > max_time_ratio_ || opts.time_ratio < 1.0 / max_time_ratio_)
            return fail(err, "time_ratio outside the prepared range [1/max, max]; "
                             "widen OfflineStretchOptions::max_time_ratio at prepare()");
        if (std::abs(opts.pitch_semitones) > max_pitch_semitones_)
            return fail(err, "pitch_semitones outside the prepared range; widen "
                             "OfflineStretchOptions::max_pitch_semitones at prepare()");

        const long expected = offline_stretch_output_frames(in_frames, opts.time_ratio);
        if (out_frames != expected)
            return fail(err, "out_frames must equal round(in_frames * time_ratio)");

        // Character-mode dispatch. `varispeed` is pitch+time-linked (tape): a pure
        // resample + a speed-scaled head EQ, no phase-vocoder artifacts. The
        // `phase_vocoder` and `granular` characters are reserved modes that fall
        // through to the clean spectral path for now (relocate_transients is a
        // documented no-op until seam-clean relocation is enabled).
        if (opts.character == StretchCharacter::varispeed)
            return varispeed_render(in, in_frames, out, out_frames, opts.time_ratio);

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

        // Route pitch==0 through the tempo-only spectral path; ratio==1 is the
        // exact identity fast path.
        const bool pitch_off = std::abs(opts.pitch_semitones) < 1e-12;
        if (pitch_off && opts.time_ratio == 1.0) {
            for (int c = 0; c < channels_; ++c)
                for (long i = 0; i < out_frames; ++i) out[c][i] = in[c][i];
            return true;
        }
        if (pitch_off) {
            if (opts.quality <= 0) { // draft: fast OLA preview
                ola_tempo(in, in_frames, out, out_frames, opts.time_ratio);
                return true;
            }
            return tempo_stretch(in, in_frames, out, out_frames, opts.time_ratio);
        }

        // Pitch-only (duration preserved): realtime_pitch engine + formant mode.
        if (opts.time_ratio == 1.0)
            return pitch_shift(in, in_frames, out, out_frames,
                               opts.pitch_semitones, opts.formant_mode, opts.formant_semitones);

        // Independent R+S (time_ratio != 1 AND pitch != 0).
        const int ch = channels_;
        const double P = std::exp2(opts.pitch_semitones / 12.0);
        if (opts.formant_mode == OfflineFormantMode::follow_pitch) {
            // Single spectral pass: stretch by R*P with pitch preserved, then
            // resample reading at step P — which shifts pitch by
            // +S, drags formants along (follow), and lands at duration R. One
            // phase-vocoder pass + one interpolation, not two cascaded PV passes.
            const double eff = opts.time_ratio * P;
            const long inter_len = offline_stretch_output_frames(in_frames, eff);
            std::vector<std::vector<float>> inter(static_cast<size_t>(ch),
                                                  std::vector<float>(static_cast<size_t>(inter_len)));
            std::vector<float*> ip(static_cast<size_t>(ch));
            for (int c = 0; c < ch; ++c) ip[c] = inter[static_cast<size_t>(c)].data();
            if (!tempo_stretch(in, in_frames, ip.data(), inter_len, eff)) return fail(err, "stretch failed");
            for (int c = 0; c < ch; ++c)
                for (long j = 0; j < out_frames; ++j)
                    out[c][j] = sample_sinc6(inter[static_cast<size_t>(c)].data(), inter_len,
                                             static_cast<double>(j) * P);
            return true;
        }

        // Formant preserve / independent: cascade pitch (formant-correct, via the
        // SpectralEnvelopeShifter) then tempo. Two phase-vocoder passes — the
        // honest cost of keeping formants fixed while both R and S change with
        // the current per-mode engine API (a true single-pass R+P needs the
        // engine-internal combination).
        std::vector<std::vector<float>> inter(static_cast<size_t>(ch),
                                              std::vector<float>(static_cast<size_t>(in_frames)));
        std::vector<float*> pp(static_cast<size_t>(ch));
        for (int c = 0; c < ch; ++c) pp[c] = inter[static_cast<size_t>(c)].data();
        if (!pitch_shift(in, in_frames, pp.data(), in_frames,
                         opts.pitch_semitones, opts.formant_mode, opts.formant_semitones))
            return fail(err, "pitch stage failed");
        std::vector<const float*> cp(static_cast<size_t>(ch));
        for (int c = 0; c < ch; ++c) cp[c] = inter[static_cast<size_t>(c)].data();
        return tempo_stretch(cp.data(), in_frames, out, out_frames, opts.time_ratio);
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

    // Varispeed (tape) render: pitch+time LINKED. Output sample i reads input
    // position i/ratio at a constant rate (a pure mastering-grade resample — no
    // phase-vocoder artifacts), then a speed-scaled head EQ colours it like a tape
    // machine at that speed. Slowing down (ratio>1) loses highs and gains low-mid
    // warmth; speeding up (ratio<1) brightens and thins. Exact identity at ratio 1.
    bool varispeed_render(const float* const* in, long in_frames,
                          float* const* out, long out_frames, double ratio) {
        for (int c = 0; c < channels_; ++c)
            for (long i = 0; i < out_frames; ++i)
                out[c][i] = sample_sinc6(in[c], in_frames,
                                         static_cast<double>(i) / ratio);
        apply_head_eq(out, out_frames, ratio);
        // Mandatory short end-fade (tape doesn't hard-cut). A bare resample of a
        // source that ends mid-ring leaves an audible tick; a ~10 ms raised-cosine
        // tail (+ 2 ms head) removes it and suits the tape character. Skipped at
        // ratio 1 to keep varispeed an exact identity there.
        if (ratio != 1.0) {
            const double sr = sample_rate_ > 0 ? sample_rate_ : 48000.0;
            const long fo = std::min<long>(static_cast<long>(0.010 * sr), out_frames / 4);
            const long fi = std::min<long>(static_cast<long>(0.002 * sr), out_frames / 4);
            constexpr double kPi = 3.14159265358979323846;
            for (int c = 0; c < channels_; ++c) {
                for (long i = 0; i < fi; ++i)
                    out[c][i] *= static_cast<float>(0.5 - 0.5 * std::cos(kPi * i / fi));
                for (long i = 0; i < fo; ++i)
                    out[c][out_frames-1-i] *= static_cast<float>(0.5 - 0.5 * std::cos(kPi * i / fo));
            }
        }
        return true;
    }

    // Speed-scaled tape head EQ: a low-mid head bump (warmth) + a head-gap HF
    // shelf (dulling), both scaled by log2(ratio) so the colour tracks the speed.
    // At ratio 1 every gain is 0 dB → exact bypass (varispeed stays a clean
    // identity at unity). Two cascaded RBJ biquads per channel, double state.
    void apply_head_eq(float* const* out, long n, double ratio) {
        if (ratio == 1.0 || n <= 0) return; // unity = transparent
        const double oct = std::log2(ratio);            // +1 per octave slower
        const double sr = sample_rate_ > 0 ? sample_rate_ : 48000.0;
        // Low-mid head bump (peaking) and head-gap HF loss (high shelf), clamped.
        const double low_db = std::clamp(2.0 * oct, -3.0, 5.0);   // warmth when slow
        const double hi_db  = std::clamp(-3.5 * oct, -10.0, 6.0); // dull when slow
        Biquad bump = Biquad::peaking(140.0, 0.8, low_db, sr);
        Biquad shelf = Biquad::high_shelf(4000.0, hi_db, sr);
        for (int c = 0; c < channels_; ++c) {
            bump.reset(); shelf.reset();
            for (long i = 0; i < n; ++i) {
                double s = static_cast<double>(out[c][i]);
                s = bump.process(s);
                s = shelf.process(s);
                out[c][i] = static_cast<float>(s);
            }
        }
    }

    // Minimal RBJ biquad (double precision) for the head EQ. Direct-form I.
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        void reset() { x1 = x2 = y1 = y2 = 0; }
        double process(double x) {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y; return y;
        }
        static Biquad from(double b0, double b1, double b2, double a0, double a1, double a2) {
            Biquad q; q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0;
            q.a1 = a1 / a0; q.a2 = a2 / a0; return q;
        }
        static Biquad peaking(double f0, double q, double gain_db, double sr) {
            const double A = std::pow(10.0, gain_db / 40.0);
            const double w0 = 2.0 * 3.14159265358979323846 * f0 / sr;
            const double cw = std::cos(w0), sw = std::sin(w0), alpha = sw / (2.0 * q);
            return from(1 + alpha * A, -2 * cw, 1 - alpha * A,
                        1 + alpha / A, -2 * cw, 1 - alpha / A);
        }
        static Biquad high_shelf(double f0, double gain_db, double sr) {
            const double A = std::pow(10.0, gain_db / 40.0);
            const double w0 = 2.0 * 3.14159265358979323846 * f0 / sr;
            const double cw = std::cos(w0), sw = std::sin(w0);
            const double alpha = sw / 2.0 * std::sqrt((A + 1 / A) * (1 / 0.9 - 1) + 2);
            const double tsa = 2.0 * std::sqrt(A) * alpha;
            return from(A * ((A + 1) + (A - 1) * cw + tsa),
                        -2 * A * ((A - 1) + (A + 1) * cw),
                        A * ((A + 1) + (A - 1) * cw - tsa),
                        (A + 1) - (A - 1) * cw + tsa,
                        2 * ((A - 1) - (A + 1) * cw),
                        (A + 1) - (A - 1) * cw - tsa);
        }
    };

    // Measure the engine's input-domain time-stretch anchor L: an impulse at P
    // synthesizes its peak at (P - L)*r + L, so L = (P*r - peak)/(r - 1). Done
    // once at prepare() so alignment is correct regardless of quality geometry
    // (fft/hop), instead of hardcoding a magic latency constant.
    double calibrate_anchor() {
        const int ch = channels_;
        const int blk = kBlock;
        const int fft = engine_.fft_size();
        const float r = 2.0f;
        const long P = static_cast<long>(fft) * 2;
        const long N = P + static_cast<long>(fft) * 6;
        engine_.reset();
        engine_.set_time_ratio(r);

        std::vector<std::vector<float>> in(static_cast<size_t>(ch),
                                           std::vector<float>(static_cast<size_t>(N), 0.0f));
        std::vector<std::vector<float>> sil(static_cast<size_t>(ch),
                                            std::vector<float>(static_cast<size_t>(blk), 0.0f));
        std::vector<std::vector<float>> ob(static_cast<size_t>(ch),
                                           std::vector<float>(static_cast<size_t>(blk)));
        for (int c = 0; c < ch; ++c) in[static_cast<size_t>(c)][static_cast<size_t>(P)] = 1.0f;
        std::vector<const float*> ip(static_cast<size_t>(ch));
        std::vector<const float*> sp(static_cast<size_t>(ch));
        std::vector<float*> op(static_cast<size_t>(ch));
        for (int c = 0; c < ch; ++c) { sp[c] = sil[c].data(); op[c] = ob[c].data(); }

        std::vector<float> acc; // channel 0 only
        auto drain = [&]() {
            while (engine_.available_stretched() > 0) {
                const int t = std::min(engine_.available_stretched(), blk);
                engine_.read_stretched(op.data(), t);
                for (int i = 0; i < t; ++i) acc.push_back(ob[0][static_cast<size_t>(i)]);
            }
        };
        for (long i = 0; i < N; i += blk) {
            for (int c = 0; c < ch; ++c) ip[c] = in[static_cast<size_t>(c)].data() + i;
            engine_.feed(ip.data(), static_cast<int>(std::min<long>(blk, N - i)));
            drain();
        }
        for (int k = 0; k < 8; ++k) { engine_.feed(sp.data(), blk); drain(); }

        long pk = 0; float mx = 0.0f;
        for (long i = 0; i < static_cast<long>(acc.size()); ++i)
            if (std::fabs(acc[static_cast<size_t>(i)]) > mx) { mx = std::fabs(acc[static_cast<size_t>(i)]); pk = i; }
        engine_.reset();
        if (mx <= 0.0f) return 0.0; // degenerate; no anchor shift
        return (static_cast<double>(P) * r - static_cast<double>(pk)) / (static_cast<double>(r) - 1.0);
    }

    // Fast draft (quality 0): plain Hann-windowed overlap-add tempo change.
    // Phase-incoherent (some smear) but quick and inoffensive — the sampler
    // shows this instantly on a tempo change and swaps in the full-quality
    // render when ready. 50%-overlap Hann is COLA (interior sums to 1), so no
    // renormalization; exact out_frames by construction; pitch preserved.
    void ola_tempo(const float* const* in, long in_frames,
                   float* const* out, long out_frames, double ratio) {
        const int ch = channels_;
        constexpr int W = 1024;
        constexpr int Hs = W / 2;
        constexpr float kTwoPi = 6.28318530717958647692f;
        const double Ha = static_cast<double>(Hs) / ratio;
        for (int c = 0; c < ch; ++c)
            std::fill(out[c], out[c] + out_frames, 0.0f);
        for (long k = 0;; ++k) {
            const long sp = k * Hs;
            if (sp >= out_frames) break;
            const long ap = static_cast<long>(std::llround(static_cast<double>(k) * Ha));
            for (int j = 0; j < W; ++j) {
                const long so = sp + j;
                if (so >= out_frames) break;
                const long ii = ap + j;
                if (ii < 0 || ii >= in_frames) continue;
                const float wnd = 0.5f * (1.0f - std::cos(kTwoPi * static_cast<float>(j)
                                                          / static_cast<float>(W)));
                for (int c = 0; c < ch; ++c) out[c][so] += in[c][ii] * wnd;
            }
        }
    }

    // Whole-file tempo-only stretch via the time_stretch engine. Front-pads with
    // silence so the real content sees full WOLA overlap, drains the stretched
    // stream, trims the stretched pad (content alignment), then copies EXACTLY
    // out_frames from the aligned start (a continuous stream => clean truncation,
    // exact length, no boundary click). Offline: allocates scratch per call. The
    // residual sub-hop coverage error vs a true distributed-hop scheduler is a
    // documented limitation of the realtime-core route.
    bool tempo_stretch(const float* const* in, long in_frames,
                       float* const* out, long out_frames, double ratio) {
        const int ch = channels_;
        const int blk = kBlock;
        const int fft = engine_.fft_size();
        engine_.reset();
        engine_.set_time_ratio(static_cast<float>(ratio));

        const long pad = static_cast<long>(std::ceil(static_cast<double>(fft) / ratio)) + fft;
        // Output index j must read real-input position j/ratio. The engine maps
        // absolute input X to stretched position (X - L)*ratio + L, where L is
        // the calibrated anchor (calibrate_anchor()); trimming this many leading
        // samples aligns real-input 0 to output 0 at every ratio.
        const long lead = static_cast<long>(std::llround(
            (static_cast<double>(pad) - latency_anchor_) * ratio + latency_anchor_));

        std::vector<std::vector<float>> accum(static_cast<size_t>(ch));
        std::vector<std::vector<float>> outblk(static_cast<size_t>(ch),
                                               std::vector<float>(static_cast<size_t>(blk)));
        std::vector<std::vector<float>> silblk(static_cast<size_t>(ch),
                                               std::vector<float>(static_cast<size_t>(blk), 0.0f));
        std::vector<const float*> inp(static_cast<size_t>(ch));
        std::vector<const float*> silp(static_cast<size_t>(ch));
        std::vector<float*> outp(static_cast<size_t>(ch));
        for (int c = 0; c < ch; ++c) { silp[c] = silblk[c].data(); outp[c] = outblk[c].data(); }

        auto drain = [&]() {
            while (engine_.available_stretched() > 0) {
                const int take = std::min(engine_.available_stretched(), blk);
                engine_.read_stretched(outp.data(), take);
                for (int c = 0; c < ch; ++c)
                    accum[static_cast<size_t>(c)].insert(
                        accum[static_cast<size_t>(c)].end(),
                        outblk[static_cast<size_t>(c)].begin(),
                        outblk[static_cast<size_t>(c)].begin() + take);
            }
        };
        auto feed = [&](const float* const* src, int n) { engine_.feed(src, n); drain(); };

        for (long p = 0; p < pad; p += blk)
            feed(silp.data(), static_cast<int>(std::min<long>(blk, pad - p)));
        for (long i = 0; i < in_frames; i += blk) {
            for (int c = 0; c < ch; ++c) inp[c] = in[c] + i;
            feed(inp.data(), static_cast<int>(std::min<long>(blk, in_frames - i)));
        }
        const long need = lead + out_frames + fft;
        const long cap = 2 * (in_frames + pad) + 64L * blk;
        for (long g = 0; static_cast<long>(accum[0].size()) < need && g < cap; g += blk)
            feed(silp.data(), blk);

        for (int c = 0; c < ch; ++c)
            for (long i = 0; i < out_frames; ++i) {
                const long idx = lead + i;
                out[c][i] = (idx >= 0 && idx < static_cast<long>(accum[static_cast<size_t>(c)].size()))
                                ? accum[static_cast<size_t>(c)][static_cast<size_t>(idx)]
                                : 0.0f;
            }
        return true;
    }

    // Duration-preserving pitch shift via the realtime_pitch engine. Pitch and
    // formant targets are set BEFORE reset() so the control smoothers latch
    // immediately (no 30 ms pitch ramp at the start). process() is equal-length
    // with a fixed leading latency, so feed input + latency silence and trim the
    // leading latency to recover exactly out_frames (== in_frames) aligned samples.
    bool pitch_shift(const float* const* in, long in_frames,
                     float* const* out, long out_frames,
                     double semitones, OfflineFormantMode fmode, double fsemis) {
        const int ch = channels_;
        const int blk = kBlock;
        pitch_engine_.set_pitch_semitones(static_cast<float>(semitones));
        switch (fmode) {
            case OfflineFormantMode::follow_pitch:
                pitch_engine_.set_formant_mode(FormantMode::follow);
                pitch_engine_.set_formant_semitones(0.0f); break;
            case OfflineFormantMode::preserve_original:
                pitch_engine_.set_formant_mode(FormantMode::preserve);
                pitch_engine_.set_formant_semitones(0.0f); break;
            case OfflineFormantMode::shift_independently:
                pitch_engine_.set_formant_mode(FormantMode::preserve);
                pitch_engine_.set_formant_semitones(static_cast<float>(fsemis)); break;
        }
        pitch_engine_.reset(); // latches the pitch/formant smoothers to the targets above

        const long lat = pitch_engine_.latency_samples();
        std::vector<std::vector<float>> accum(static_cast<size_t>(ch));
        std::vector<std::vector<float>> ob(static_cast<size_t>(ch),
                                           std::vector<float>(static_cast<size_t>(blk)));
        std::vector<std::vector<float>> sil(static_cast<size_t>(ch),
                                            std::vector<float>(static_cast<size_t>(blk), 0.0f));
        std::vector<const float*> ip(static_cast<size_t>(ch));
        std::vector<const float*> sp(static_cast<size_t>(ch));
        std::vector<float*> op(static_cast<size_t>(ch));
        for (int c = 0; c < ch; ++c) { sp[c] = sil[c].data(); op[c] = ob[c].data(); }

        auto pump = [&](const float* const* src, int nn) {
            pitch_engine_.process(src, op.data(), nn);
            for (int c = 0; c < ch; ++c)
                accum[static_cast<size_t>(c)].insert(
                    accum[static_cast<size_t>(c)].end(),
                    ob[static_cast<size_t>(c)].begin(),
                    ob[static_cast<size_t>(c)].begin() + nn);
        };
        for (long i = 0; i < in_frames; i += blk) {
            for (int c = 0; c < ch; ++c) ip[c] = in[c] + i;
            pump(ip.data(), static_cast<int>(std::min<long>(blk, in_frames - i)));
        }
        for (long f = 0; f < lat; f += blk)
            pump(sp.data(), static_cast<int>(std::min<long>(blk, lat - f)));

        for (int c = 0; c < ch; ++c)
            for (long i = 0; i < out_frames; ++i) {
                const long idx = lat + i;
                out[c][i] = (idx >= 0 && idx < static_cast<long>(accum[static_cast<size_t>(c)].size()))
                                ? accum[static_cast<size_t>(c)][static_cast<size_t>(idx)]
                                : 0.0f;
            }
        return true;
    }

    static bool fail(std::string* err, const char* msg) {
        if (err) *err = msg;
        return false;
    }

    static constexpr int kBlock = 4096;
    RealtimePitchTimeProcessor engine_;
    RealtimePitchTimeProcessor pitch_engine_;
    double latency_anchor_ = 0.0;
    double sample_rate_ = 0.0;
    int channels_ = 1;
    double max_time_ratio_ = 4.0;
    double max_pitch_semitones_ = 24.0;
    int fft_size_ = 0;
    bool prepared_ = false;
};

} // namespace pulp::signal
