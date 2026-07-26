#pragma once

/// @file vocoder.hpp
/// The classic channel vocoder — Dudley's analysis-synthesis speech remaker.
///
/// A bank of bandpass filters measures a *modulator* (usually a voice) as a
/// coarse, slowly-varying spectral envelope; a **matched** bank imposes that
/// envelope on a spectrally rich *carrier*, one gain per band. The carrier
/// speaks. That is the whole idea, and it is Homer Dudley's: reconstruct speech
/// from a buzz source, a hiss source, and a set of band levels.
///
/// Three things separate a vocoder that works from one that does not, and all
/// three are here rather than in a post-process:
///
///   * **The two banks are matched.** Analysis band `k` and synthesis band `k`
///     share one centre frequency and one section Q, computed once. If they
///     drift apart the reconstruction is a smeared, detuned version of the
///     modulator's formants — the classic vocoder bug, and one that sounds like
///     a tuning problem rather than a filter problem, which is why the
///     acceptance suite measures both banks against the same shipped `f_c[k]`.
///   * **Voiced and unvoiced excitation are different sources.** Dudley's buzz
///     and hiss. A vocoder with only a pitched carrier turns every "s", "t" and
///     "f" into a pitched chirp, and speech stops being intelligible long
///     before it stops being recognisable.
///   * **The band ballistics are per band, not global.** A rectified sinusoid
///     at `f_c` ripples at `2·f_c`; a follower fast enough for the top of the
///     bank re-imposes that ripple as amplitude modulation at the bottom of it.
///     The floors below are expressed in *cycles of each band's own centre*, so
///     one pair of user controls behaves correctly across the whole bank.
///
/// ## Lineage (documented behaviour, concepts and band counts only)
///
/// Roland VP-330 (10-band vocoder in an ensemble bed), Moog 16-Channel Vocoder
/// / Bode 7702 (16 bands, analysis→synthesis cross-matrix — the ancestor of
/// `set_formant_shift_semitones`), Korg DVP-1 (a vocoder with its own internal
/// carrier, which is why `CarrierSource::internal` exists). No filter
/// coefficients, envelope constants or code from any of them are used.
///
/// ## Composition — what this module does NOT own
///
///   * **The bandpass sections.** `SvfT` in bandpass mode, two cascaded per
///     band per bank. Note the normalisation: a TPT SVF's bandpass output has
///     peak gain `Q`, not 1, and two identical sections at one centre peak
///     together at `Q²`, so each band is scaled by `1/Q_section²`. Every gain
///     statement in this file assumes unity-peak bands, and the acceptance
///     suite measures that peak rather than trusting the algebra — the first
///     draft of this file normalised once and shipped bands peaking at 2.371.
///   * **The followers.** `BallisticsFilterT` in peak mode, which rectifies
///     internally. Its time argument is a **10→90 % time**, not a time
///     constant — its shipped coefficient map is `1 − exp(−2.2/(t·fs))`, and
///     2.2 is `ln 9` rounded. Anything in this file that says "attack" or
///     "release" in milliseconds means that convention, and `kTenToNinety`
///     reuses the same 2.2 so the two never disagree. Note that the spec
///     describes the map as `exp(−1/(τ·fs))`, a time-CONSTANT convention; the
///     shipped primitive does not do that, and the difference is a factor of
///     2.2 in every follower time.
///   * **The per-band gain law.** `VcaT` with `Response::linear`, because a
///     band envelope is already a magnitude; an exponential law would square a
///     quantity that was already the answer. Its clamp of the control to
///     `[0, 1]` is load-bearing here, not incidental — it is what makes the
///     reconstruction bound in `kWorstCaseGain` a bound rather than a hope.
///   * **The internal carrier.** `osc::VaOscillator`, polyBLEP-corrected.
///   * **Noise, DC blocking, the sibilance high-pass.** `Xorshift32`,
///     `DcBlocker`, `TptFilterT`.
///   * **The ensemble tail.** Deliberately absent — see the note below.
///
/// ## What is deliberately NOT here: the ensemble tail
///
/// The VP-330's signature is a vocoded voice sitting in a wide detuned bed, and
/// the spec is explicit that this class owns no chorus state: the catalog node
/// wires this module's mono output into a `ChorusEnsembleT` and crossfades by
/// its own `ensemble_amt`. So there is no `set_ensemble_amt()` here and no
/// include of `chorus_family.hpp`. The composition is not untested for being
/// absent — the acceptance suite builds the node's wiring explicitly and
/// asserts it, so the node author has an executable reference rather than a
/// paragraph.
///
/// ## Constants: published vs design parameter
///
/// Published and cited at the constant: none of the *numbers* here are, and
/// that is the honest position. What is published is the **mechanism** — the
/// buzz/hiss excitation split and slowly-varying band levels (Dudley 1939),
/// zero-crossing rate and high-frequency energy as voicing cues plus
/// pre-emphasis (Flanagan 1972), quasi-logarithmic critical-band spacing as the
/// reason to place centres geometrically (Zwicker 1961), and the TPT SVF
/// topology (Zavalishin). Every numeric value below is a design parameter with
/// a default and a range, or a derived identity (`kCascadeBWFactor`, `Q_band`)
/// that is algebra on the shipped spacing rather than a citation. The classic
/// band counts are documented behaviour, not sourced coefficients.
///
/// ## Anti-aliasing policy (series law 4)
///
/// **None required, and the reason is structural rather than fortunate.** The
/// signal path contains exactly two nonlinearities and neither reaches the
/// output as broadband audio: the follower's rectifier feeds a one-pole
/// lowpass, not the output, and the internal oscillator is band-limited by
/// polyBLEP construction. The per-band multiply is a product of an audio signal
/// with a control signal whose bandwidth is bounded by the follower — a
/// ring-modulation whose sidebands sit within a few hundred Hz of the band they
/// belong to, far from Nyquist. Nothing here would benefit from oversampling.
///
/// ## Latency (series law 5)
///
/// `latency_samples()` is **0**. Every stage is minimum-phase IIR — TPT
/// bandpass sections, one-pole followers, one-zero pre-emphasis, first-order
/// high-passes. The banks impose frequency-dependent group delay, which is
/// inherent to IIR filtering and is not a compensable bulk delay, so there is
/// nothing to report and an impulse produces output on sample 0.
///
/// RT contract: `prepare(sample_rate)` computes coefficients and clears state;
/// it allocates nothing, because every buffer is a fixed `std::array` sized at
/// construction for `kMaxBands` and for the widest ZCR window at the highest
/// supported rate. `set_*`, `process` and `reset` allocate nothing, take no
/// locks and perform no I/O. `set_band_count`/`set_band_range_hz` recompute
/// filter coefficients and follower floors — control-rate work, still
/// allocation-free, and they change only the active loop bound, never the
/// storage. Randomness is one `Xorshift32` rewound by `reset()` to
/// `kNoiseSeed`, and one sample is drawn per `process()` call regardless of
/// carrier source, so a render is bit-identical per (params, input) and does
/// not depend on which branch ran.

#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/osc/va.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/svf.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/vca.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// Where the thing that speaks comes from.
enum class VocoderCarrier : std::uint8_t {
    external,  ///< The caller's `carrier_ext` input: a pad, a guitar, anything rich.
    internal,  ///< The module's own band-limited oscillator — the DVP-1 lesson.
};

/// The internal carrier's shape. Both are polyBLEP-corrected.
enum class VocoderWave : std::uint8_t { saw, pulse };

/// A classic channel vocoder: analysis bank → envelopes → matched synthesis
/// bank. Mono in, mono out, one sample at a time.
template <typename SampleType = float>
class VocoderT {
public:
    using CarrierSource = VocoderCarrier;
    using InternalWave = VocoderWave;

    // ── Structural constants ──────────────────────────────────────────────

    /// Storage cap. Equals the `band_count` maximum, so it is not an
    /// independent tunable: the array is sized for the widest bank the
    /// parameter allows and `set_band_count` moves only the loop bound.
    static constexpr int kMaxBands = 20;
    static constexpr int kMinBands = 10;
    static constexpr std::size_t kBandSlots = static_cast<std::size_t>(kMaxBands);

    /// DC-blocker corner for both the modulator and the carrier path, Hz.
    ///
    /// Not cosmetic: `DcBlocker`'s own default pole puts the corner near 38 Hz
    /// at 48 kHz, which is *inside* the bank — it costs band 0 4.5 % at the
    /// 120 Hz default and 16 % at the 60 Hz the parameter table allows. The
    /// corner has to sit well below `kFreqLoMinHz`, and it has to be specified
    /// in Hz rather than as a pole so it means the same thing at every sample
    /// rate.
    /// [design parameter] default 10 Hz, range 2 .. 30 Hz.
    static constexpr double kDcBlockHz = 10.0;

    /// Highest sample rate the fixed ZCR window is sized for. A higher rate
    /// still runs; the window simply saturates at `kZcrMaxSamples`, which
    /// shortens it in milliseconds — reported by `zcr_window_ms()` rather than
    /// silently assumed. [design parameter] default 192 kHz, range 48–768 kHz.
    static constexpr double kMaxSampleRate = 192000.0;

    // ── Band splitting ────────────────────────────────────────────────────

    /// Bandwidth factor for two identical cascaded 2nd-order bandpass sections.
    ///
    /// Derived identity, not a citation. For `n` identical cascaded sections,
    /// `|H|^n = 1/√2` at the cascade's −3 dB edges gives `1 + x² = 2^(1/n)`
    /// where `x = Q_section·(ω/ω_c − ω_c/ω)`, so the cascade's −3 dB bandwidth
    /// is narrower than one section's by `√(2^(1/n) − 1)`. For `n = 2`:
    /// `√(√2 − 1) = 0.6435942...`. Each section therefore runs at
    /// `Q_section = kCascadeBWFactor · Q_band`, i.e. deliberately *broader*
    /// than the target, so that the cascade lands on it. The acceptance suite
    /// recomputes this from `√(√2 − 1)` rather than trusting the literal.
    static constexpr double kCascadeBWFactor = 0.6435942529055827;

    /// Pre-emphasis coefficient, `y[n] = x[n] − a·x[n−1]`, applied to the
    /// modulator only. Speech rolls off about −6 dB/oct, so without this the
    /// upper bands see almost nothing and consonants never earn a fair envelope
    /// level. [design parameter] default 0.95, range 0.0 (off) .. 0.97.
    static constexpr double kPreEmphasis = 0.95;

    // ── Follower floors ───────────────────────────────────────────────────

    /// Release floor, in cycles of each band's own centre frequency. The
    /// *mechanism* is textbook — a rectified sinusoid at `f_c` ripples at
    /// `2·f_c` — but the count is an original engineering choice with no
    /// citable constant behind it (honest gap), which is why it ships as a
    /// named parameter rather than as a number inside a formula.
    /// [design parameter] default 2.0, range 1.0 .. 4.0.
    static constexpr double kRippleCycles = 2.0;

    /// Attack floor, same units and the same honest gap.
    /// [design parameter] default 0.5, range 0.25 .. 1.0.
    static constexpr double kAttackCycles = 0.5;

    // ── Voiced / unvoiced detection ───────────────────────────────────────

    /// Zero-crossing rate mapped to `[0, 1]` between these edges. Voiced speech
    /// crosses slowly; fricatives cross fast. The cue is Flanagan's; the edges
    /// are tuned design parameters (honest gap — no citable threshold exists).
    /// [design parameter] default 1500 Hz, range 800 .. 2000 Hz.
    static constexpr double kZcrLo = 1500.0;
    /// [design parameter] default 4500 Hz, range 3000 .. 6000 Hz.
    static constexpr double kZcrHi = 4500.0;
    /// Sliding-window length for the crossing count.
    /// [design parameter] default 20 ms, range 10 .. 40 ms.
    static constexpr double kZcrWindowMs = 20.0;

    /// What counts as sibilance. Single-sourced deliberately: the same corner
    /// decides which bands feed the high-frequency energy ratio, which bands
    /// take noise substitution under an external carrier, and where the direct
    /// sibilance path starts. Three definitions of "high" would drift apart.
    /// [design parameter] default 3500 Hz, range 2500 .. 5000 Hz.
    static constexpr double kSibilanceCornerHz = 3500.0;

    /// Schmitt thresholds on the raw decision. `kUvEnter > kUvLeave` always.
    /// [design parameter] defaults 0.6 / 0.4, ranges 0.55 .. 0.75 and
    /// 0.25 .. 0.45.
    static constexpr double kUvEnter = 0.6;
    static constexpr double kUvLeave = 0.4;

    /// How far `unvoiced_sensitivity` can bias the raw decision.
    /// [design parameter] default 0.5, range 0.3 .. 0.7.
    static constexpr double kSensSpan = 0.5;

    /// One-pole smoothing on the decision, as a **10→90 % time** to match the
    /// follower convention this file uses everywhere.
    /// [design parameter] default 8 ms, range 2 .. 30 ms.
    static constexpr double kUvSmoothMs = 8.0;

    /// Guards the high-frequency energy ratio's divide when every band is
    /// silent. Any value far below the quietest meaningful band envelope works;
    /// range 1e-15 .. 1e-9.
    static constexpr double kEnergyEpsilon = 1e-12;

    // ── Carrier ───────────────────────────────────────────────────────────

    /// How much noise a fully unvoiced decision substitutes into the carrier.
    /// [design parameter] default 0.9, range 0.5 .. 1.0.
    static constexpr double kUnvoicedNoise = 0.9;

    /// Noise seed. Any non-zero 32-bit value works — `Xorshift32` has a fixed
    /// point at zero — so the range is the whole non-zero `uint32` space. Per
    /// series law 2 it is a build-time constant, never automated, never
    /// macro-exposed, never randomised at run time.
    /// [design parameter] default 0x9E3779B9.
    static constexpr std::uint32_t kNoiseSeed = 0x9E3779B9u;

    /// Internal carrier pitch, Hz.
    ///
    /// **Not in the spec, and it has to be.** The spec gives the internal
    /// oscillator a wave and a pulse width but no frequency — neither in the
    /// class API nor in the baked-params table — while saying pitch "is set by
    /// the host/MIDI upstream". There is no upstream to set it through, and the
    /// module cannot make a sound without one, so this is added with a default
    /// and a range and reported as a spec gap.
    /// [design parameter] default 110 Hz (A2), range 20 .. 2000 Hz.
    static constexpr double kDefaultPitchHz = 110.0;
    static constexpr double kMinPitchHz = 20.0;
    static constexpr double kMaxPitchHz = 2000.0;

    // ── Output ────────────────────────────────────────────────────────────

    /// Static trim on the summed synthesis bank. Overlapping constant-Q bands
    /// sum with partial coherence; this is the fixed allowance for it.
    /// [design parameter] default 0.5 (−6.02 dB), range 0.35 .. 0.7.
    static constexpr double kOutputHeadroomTrim = 0.5;

    /// The pre-trim reconstruction bound Forge's registry cites. Fixed by the
    /// filterbank topology (constant-Q sections, up to `kMaxBands` overlapping)
    /// rather than by a knob, and asserted by the acceptance suite at every
    /// band count — never estimated (series law 8).
    static constexpr double kWorstCaseGain = 2.0;

    // ── Parameter bounds (the baked-params table, in one place) ───────────

    static constexpr double kFreqLoMinHz = 60.0;
    static constexpr double kFreqLoMaxHz = 400.0;
    static constexpr double kFreqHiMinHz = 3000.0;
    static constexpr double kFreqHiMaxHz = 12000.0;
    static constexpr double kAttackMinMs = 0.1;
    static constexpr double kAttackMaxMs = 50.0;
    static constexpr double kReleaseMinMs = 2.0;
    static constexpr double kReleaseMaxMs = 200.0;
    static constexpr double kPulseWidthMin = 0.05;
    static constexpr double kPulseWidthMax = 0.95;
    static constexpr double kFormantShiftMinSt = -24.0;
    static constexpr double kFormantShiftMaxSt = 24.0;
    static constexpr double kOutputTrimMinDb = -24.0;
    static constexpr double kOutputTrimMaxDb = 12.0;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    VocoderT() { rebuild(); }

    /// Fixes the sample rate, recomputes every coefficient, and clears state.
    void prepare(double sample_rate) {
        sample_rate_ = (std::isfinite(sample_rate) && sample_rate > 0.0) ? sample_rate : 48000.0;
        rebuild();
        reset();
    }

    void reset() {
        modulator_dc_.reset();
        carrier_dc_low_.reset();
        carrier_dc_high_.reset();
        sibilance_hp_.reset();
        pre_emphasis_state_ = 0.0;
        oscillator_.reset(0.0);
        rng_.reset();

        zcr_bits_.fill(0u);
        zcr_position_ = 0;
        zcr_count_ = 0;
        zcr_hz_ = 0.0;
        previous_sign_ = true;

        unvoiced_ = 0.0;
        unvoiced_latched_ = false;
        freeze_edge_ = false;

        for (std::size_t k = 0; k < kBandSlots; ++k) {
            bands_[k].analysis_a.reset();
            bands_[k].analysis_b.reset();
            bands_[k].synthesis_a.reset();
            bands_[k].synthesis_b.reset();
            bands_[k].follower.reset();
            bands_[k].analysis_out = 0.0;
            bands_[k].envelope = 0.0;
            bands_[k].held = 0.0;
            bands_[k].gain = 0.0;
        }
    }

    /// 0 for every configuration. See this file's latency note.
    int latency_samples() const { return 0; }

    // ── Configuration ─────────────────────────────────────────────────────

    /// Band count, 10 .. 20. Re-derives the whole bank; allocation-free, and it
    /// moves only the active loop bound.
    void set_band_count(int n) {
        band_count_ = std::clamp(n, kMinBands, kMaxBands);
        rebuild();
    }

    /// Analysis and synthesis span. `f_hi` is additionally held above `f_lo` so
    /// the geometric ratio stays greater than 1.
    void set_band_range_hz(double f_lo, double f_hi) {
        if (!std::isfinite(f_lo) || !std::isfinite(f_hi)) return;
        freq_lo_ = std::clamp(f_lo, kFreqLoMinHz, kFreqLoMaxHz);
        freq_hi_ = std::clamp(f_hi, kFreqHiMinHz, kFreqHiMaxHz);
        if (freq_hi_ <= freq_lo_ * 2.0) freq_hi_ = freq_lo_ * 2.0;
        rebuild();
    }

    void set_carrier_source(CarrierSource source) { carrier_source_ = source; }
    CarrierSource carrier_source() const { return carrier_source_; }

    void set_internal_wave(InternalWave wave) {
        internal_wave_ = wave;
        oscillator_.set_shape(wave == InternalWave::saw ? osc::VaShape::saw
                                                        : osc::VaShape::square);
    }

    void set_internal_pulse_width(SampleType w01) {
        if (!std::isfinite(static_cast<double>(w01))) return;
        pulse_width_ = std::clamp(static_cast<double>(w01), kPulseWidthMin, kPulseWidthMax);
        oscillator_.set_pulse_width(pulse_width_);
    }

    /// The internal carrier's frequency. See `kDefaultPitchHz` for why this
    /// exists when the spec has no such control.
    void set_internal_pitch_hz(SampleType hz) {
        if (!std::isfinite(static_cast<double>(hz))) return;
        pitch_hz_ = std::clamp(static_cast<double>(hz), kMinPitchHz, kMaxPitchHz);
    }

    double internal_pitch_hz() const { return pitch_hz_; }

    void set_noise_mix(SampleType m01) { noise_mix_ = clamp_unit(m01, noise_mix_); }

    /// Follower attack as a **10→90 % time** in ms, floored per band.
    void set_attack_ms(SampleType ms) {
        if (!std::isfinite(static_cast<double>(ms))) return;
        attack_ms_ = std::clamp(static_cast<double>(ms), kAttackMinMs, kAttackMaxMs);
        apply_ballistics();
    }

    /// Follower release as a **10→90 % time** in ms, floored per band.
    void set_release_ms(SampleType ms) {
        if (!std::isfinite(static_cast<double>(ms))) return;
        release_ms_ = std::clamp(static_cast<double>(ms), kReleaseMinMs, kReleaseMaxMs);
        apply_ballistics();
    }

    void set_unvoiced_sensitivity(SampleType s01) {
        unvoiced_sensitivity_ = clamp_unit(s01, unvoiced_sensitivity_);
    }

    void set_sibilance_mix(SampleType m01) { sibilance_mix_ = clamp_unit(m01, sibilance_mix_); }

    /// Formant shift in semitones, converted to a fractional band-index offset
    /// through the bank's own ratio, so the control means the same thing at
    /// every band count and every span.
    void set_formant_shift_semitones(SampleType st) {
        if (!std::isfinite(static_cast<double>(st))) return;
        formant_shift_st_ =
            std::clamp(static_cast<double>(st), kFormantShiftMinSt, kFormantShiftMaxSt);
        update_shift();
    }

    /// Latches the band levels. The latch is a value copy taken on the rising
    /// edge inside `process`, so it is deterministic and needs no randomness.
    /// Followers keep running while frozen; their output is simply ignored.
    void set_formant_freeze(bool on) { freeze_ = on; }
    bool formant_freeze() const { return freeze_; }

    void set_output_trim_db(SampleType db) {
        if (!std::isfinite(static_cast<double>(db))) return;
        output_trim_db_ =
            std::clamp(static_cast<double>(db), kOutputTrimMinDb, kOutputTrimMaxDb);
        output_trim_ = units::db_to_linear(output_trim_db_);
    }

    /// 0 = the (DC-blocked) modulator passes through, 1 = fully vocoded.
    ///
    /// The spec contradicts itself here: this setter is in its declared class
    /// API, while its node-wiring note says dry/wet lives in node code and not
    /// in this template. Resolved in favour of the declared API — a node that
    /// wants to own the crossfade simply leaves this at 1.0, whereas a caller
    /// of a class that dropped a declared method has no recourse. Note that the
    /// `out_dry` parameter of `process` is unrelated: it means "before the
    /// node's chorus", not "the dry signal".
    void set_dry_wet(SampleType w01) { dry_wet_ = clamp_unit(w01, dry_wet_); }

    // ── Audio ─────────────────────────────────────────────────────────────

    void process(SampleType modulator, SampleType carrier_ext, SampleType& out_dry) {
        // Both inputs feed recursive filters. A non-finite sample is a recovery
        // boundary: reject it before it can latch the filterbank, oscillator,
        // voicing detector, or freeze state.
        if (!std::isfinite(static_cast<double>(modulator)) ||
            !std::isfinite(static_cast<double>(carrier_ext))) {
            reset();
            out_dry = SampleType{0};
            return;
        }
        const double modulator_in = static_cast<double>(modulator);
        const double blocked = modulator_dc_.process(modulator_in);

        // Pre-emphasis lifts the upper bands so consonants earn a fair envelope
        // level. Modulator only — never the carrier, never the output.
        const double emphasised = blocked - kPreEmphasis * pre_emphasis_state_;
        pre_emphasis_state_ = blocked;

        update_zero_crossings(blocked);

        // ── Analysis bank ─────────────────────────────────────────────────
        double energy_total = 0.0;
        double energy_high = 0.0;
        for (std::size_t k = 0; k < active_; ++k) {
            Band& band = bands_[k];
            const double filtered =
                band.analysis_b.process(band.analysis_a.process(emphasised)) * band.normalisation;
            band.analysis_out = filtered;
            band.envelope = band.follower.process(filtered);
            energy_total += band.envelope;
            if (band.center_hz > kSibilanceCornerHz) energy_high += band.envelope;
        }

        // ── Voiced / unvoiced ─────────────────────────────────────────────
        const double zcr_norm =
            std::clamp((zcr_hz_ - kZcrLo) / (kZcrHi - kZcrLo), 0.0, 1.0);
        const double hf_ratio = energy_high / (energy_total + kEnergyEpsilon);
        const double raw = std::clamp(0.5 * zcr_norm + 0.5 * hf_ratio -
                                          (0.5 - unvoiced_sensitivity_) * kSensSpan,
                                      0.0, 1.0);
        // Schmitt on the raw decision, then a one-pole on the latched result.
        // That order is what lets `u` actually reach 0 and 1: the raw cue tops
        // out well below 1 even for full-band noise (its high-frequency energy
        // ratio is bounded by how much of the bank sits above the sibilance
        // corner), so smoothing the raw value would leave `u` permanently
        // mid-scale. The hysteresis decides, and the one-pole keeps the
        // decision from arriving as a click.
        if (raw > kUvEnter) unvoiced_latched_ = true;
        else if (raw < kUvLeave) unvoiced_latched_ = false;
        unvoiced_ += unvoiced_coefficient_ * ((unvoiced_latched_ ? 1.0 : 0.0) - unvoiced_);

        // ── Freeze, then shift ────────────────────────────────────────────
        // Freeze first so a frozen vowel can still be swept with the formant
        // control — the "hold a vowel and morph ooh→aah" gesture needs the
        // shift to act on held levels, not to be locked out by them.
        if (freeze_ && !freeze_edge_)
            for (std::size_t k = 0; k < active_; ++k) bands_[k].held = bands_[k].envelope;
        freeze_edge_ = freeze_;

        for (std::size_t k = 0; k < active_; ++k)
            source_[k] = freeze_ ? bands_[k].held : bands_[k].envelope;
        for (std::size_t j = 0; j < active_; ++j)
            bands_[j].gain = shifted_level(static_cast<double>(j) - shift_bands_);

        // ── Carrier ───────────────────────────────────────────────────────
        // One noise draw per call, unconditionally, so the generator's position
        // does not depend on the carrier source or on the voicing decision.
        // A render is then bit-identical for a given (params, input) no matter
        // which branches ran.
        const double noise = rng_.next_bipolar<double>();
        const double increment = pitch_hz_ / sample_rate_;
        const double oscillator = oscillator_.next(increment);

        const double raw_carrier = carrier_source_ == CarrierSource::internal
                                       ? oscillator
                                       : static_cast<double>(carrier_ext);

        // Internal: noise blends into the whole carrier. External: the carrier
        // is the user's and stays intact in the lower bands, with noise
        // substituted only above the sibilance corner so consonants read
        // without smearing the pad the caller chose.
        const double unvoiced_noise = unvoiced_ * kUnvoicedNoise;
        const double internal_noise = std::max(noise_mix_, unvoiced_noise);
        const bool internal = carrier_source_ == CarrierSource::internal;
        const double mixed_low =
            internal ? (1.0 - internal_noise) * raw_carrier + internal_noise * noise : raw_carrier;
        const double mixed_high =
            internal ? mixed_low : (1.0 - unvoiced_noise) * raw_carrier + unvoiced_noise * noise;

        // DC-block the SUMMED carrier, after the blend rather than before it —
        // a pulse carrier's DC moves with its width, and the noise term needs
        // the same treatment as the oscillator it replaces. Both blockers run
        // every sample whatever the source, so neither one's state depends on
        // which branch was taken.
        const double carrier_low = carrier_dc_low_.process(mixed_low);
        const double carrier_high = carrier_dc_high_.process(mixed_high);

        // ── Synthesis bank ────────────────────────────────────────────────
        double sum = 0.0;
        for (std::size_t k = 0; k < active_; ++k) {
            Band& band = bands_[k];
            const double input = band.center_hz > kSibilanceCornerHz ? carrier_high : carrier_low;
            const double filtered =
                band.synthesis_b.process(band.synthesis_a.process(input)) * band.normalisation;
            sum += vca_.process(filtered, band.gain);
        }

        double wet = kOutputHeadroomTrim * sum;
        // Gated by `u` as well as by the user control: an ungated sibilance
        // path leaks breath through every sustained vowel.
        wet += sibilance_mix_ * unvoiced_ * sibilance_hp_.process_highpass(blocked);
        wet *= output_trim_;

        const double output = (1.0 - dry_wet_) * blocked + dry_wet_ * wet;
        if (!std::isfinite(output)) {
            reset();
            out_dry = SampleType{0};
            return;
        }
        out_dry = static_cast<SampleType>(output);
    }

    // ── Observability ─────────────────────────────────────────────────────
    //
    // The quantities this module is judged on — band centres, per-band
    // envelopes, the voicing decision, the gains actually applied — are not
    // visible in the output, so they are exposed. The acceptance suite still
    // proves the bank against audio (it measures both filterbanks with coherent
    // DFTs of the accessor's own output), rather than letting the module agree
    // with itself.

    int band_count() const { return band_count_; }
    double band_ratio() const { return ratio_; }
    double band_q() const { return q_band_; }
    double section_q() const { return q_section_; }
    double bands_per_octave() const { return 1.0 / std::log2(ratio_); }
    double shift_bands() const { return shift_bands_; }

    double band_center_hz(int k) const { return in_range(k) ? bands_[index(k)].center_hz : 0.0; }
    double attack_eff_ms(int k) const { return in_range(k) ? bands_[index(k)].attack_ms : 0.0; }
    double release_eff_ms(int k) const { return in_range(k) ? bands_[index(k)].release_ms : 0.0; }

    /// The 4th-order analysis output for band `k` on the most recent sample —
    /// the filter's own signal, before rectification.
    double analysis_band(int k) const { return in_range(k) ? bands_[index(k)].analysis_out : 0.0; }

    /// The live follower output for band `k`, ignoring freeze and shift.
    double band_envelope(int k) const { return in_range(k) ? bands_[index(k)].envelope : 0.0; }

    /// The level actually applied to synthesis band `k` — post-freeze,
    /// post-shift. This is `env'[k]`.
    double synthesis_gain(int k) const { return in_range(k) ? bands_[index(k)].gain : 0.0; }

    /// The smoothed voicing decision: 0 fully voiced, 1 fully unvoiced.
    double unvoiced() const { return unvoiced_; }

    /// Measured zero-crossing rate of the modulator, Hz.
    double zcr_hz() const { return zcr_hz_; }

    /// Effective ZCR window length in ms. Equals `kZcrWindowMs` unless the
    /// sample rate exceeds `kMaxSampleRate`, in which case the fixed buffer
    /// shortens it and this reports the truth.
    double zcr_window_ms() const {
        return 1000.0 * static_cast<double>(zcr_length_) / sample_rate_;
    }

private:
    struct Band {
        SvfT<double> analysis_a;
        SvfT<double> analysis_b;
        SvfT<double> synthesis_a;
        SvfT<double> synthesis_b;
        BallisticsFilterT<double> follower;
        double center_hz = 0.0;
        /// `1/Q_section` for EACH of the two cascaded sections, folded into one
        /// multiply — so `1/Q_section²`, not `1/Q_section`. A TPT SVF's
        /// bandpass output peaks at `Q`, not 1, and two identical sections at
        /// the same centre peak together at `Q²`. Every gain claim in this file
        /// assumes unity-peak bands, and the acceptance suite measures that
        /// peak: the first draft of this file normalised once and shipped bands
        /// peaking at 2.371.
        double normalisation = 1.0;
        double attack_ms = 0.0;
        double release_ms = 0.0;
        double analysis_out = 0.0;
        double envelope = 0.0;
        double held = 0.0;
        double gain = 0.0;
    };

    static constexpr std::size_t kZcrMaxSamples =
        static_cast<std::size_t>(40.0 * 0.001 * kMaxSampleRate);  // widest window, highest rate
    static constexpr std::size_t kZcrWords = (kZcrMaxSamples + 63u) / 64u;

    bool in_range(int k) const { return k >= 0 && k < band_count_; }
    static std::size_t index(int k) { return static_cast<std::size_t>(k); }

    double clamp_unit(SampleType v, double fallback) const {
        const double d = static_cast<double>(v);
        return std::isfinite(d) ? std::clamp(d, 0.0, 1.0) : fallback;
    }

    /// Recomputes the whole bank: ratio, both Qs, every centre, both banks'
    /// coefficients, and the per-band follower floors.
    void rebuild() {
        active_ = static_cast<std::size_t>(band_count_);
        const double span = std::max(freq_hi_ / freq_lo_, 1.0000001);
        ratio_ = std::pow(span, 1.0 / static_cast<double>(band_count_ - 1));

        const double root = std::sqrt(ratio_);
        q_band_ = 1.0 / (root - 1.0 / root);
        q_section_ = kCascadeBWFactor * q_band_;

        // A centre above Nyquist is not a band, so the span is held below it.
        // At every rate the parameter table allows this is inactive; it exists
        // so a 22.05 kHz render degrades to a narrower bank instead of to
        // garbage coefficients.
        const double ceiling = 0.45 * sample_rate_;
        for (std::size_t k = 0; k < kBandSlots; ++k) {
            Band& band = bands_[k];
            const double nominal = freq_lo_ * std::pow(ratio_, static_cast<double>(k));
            band.center_hz = std::min(nominal, ceiling);
            band.normalisation = 1.0 / (q_section_ * q_section_);
            for (auto* section : {&band.analysis_a, &band.analysis_b, &band.synthesis_a,
                                  &band.synthesis_b}) {
                section->set_mode(SvfT<double>::Mode::bandpass);
                section->set_sample_rate(sample_rate_);
                section->set_resonance(q_section_);
                section->set_frequency(band.center_hz);
            }
            band.follower.set_mode(BallisticsFilterT<double>::Mode::peak);
            band.follower.prepare(sample_rate_);
        }

        apply_ballistics();
        update_shift();

        sibilance_hp_.prepare(sample_rate_);
        sibilance_hp_.set_cutoff(kSibilanceCornerHz);

        const double dc_pole = std::exp(-2.0 * kPiConstant * kDcBlockHz / sample_rate_);
        modulator_dc_.set_pole(dc_pole);
        carrier_dc_low_.set_pole(dc_pole);
        carrier_dc_high_.set_pole(dc_pole);

        zcr_length_ = static_cast<std::size_t>(
            std::llround(std::clamp(kZcrWindowMs * 0.001 * sample_rate_, 1.0,
                                    static_cast<double>(kZcrMaxSamples))));

        // 10→90 % time on the voicing decision, using the SAME constant the
        // composed follower uses, so both "10→90 % time" figures in this file
        // mean exactly the same thing rather than differing by a rounding.
        unvoiced_coefficient_ =
            1.0 - std::exp(-kTenToNinety / (kUvSmoothMs * 0.001 * sample_rate_));

        oscillator_.set_shape(internal_wave_ == InternalWave::saw ? osc::VaShape::saw
                                                                 : osc::VaShape::square);
        oscillator_.set_pulse_width(pulse_width_);
        vca_.set_response(VcaResponse::linear);
        output_trim_ = units::db_to_linear(output_trim_db_);
    }

    /// Floors each band's ballistics to a fixed number of cycles of its own
    /// centre. Low bands are forced slow and high bands stay snappy, from one
    /// pair of user controls and no extra knobs.
    void apply_ballistics() {
        for (std::size_t k = 0; k < kBandSlots; ++k) {
            Band& band = bands_[k];
            const double center = std::max(band.center_hz, 1.0);
            band.attack_ms = std::max(attack_ms_, 1000.0 * kAttackCycles / center);
            band.release_ms = std::max(release_ms_, 1000.0 * kRippleCycles / center);
            band.follower.set_attack_ms(band.attack_ms);
            band.follower.set_release_ms(band.release_ms);
        }
    }

    /// Semitones → fractional band offset, through the bank's own ratio. A
    /// shift of one octave is `1/log2(r)` bands whatever the bank looks like,
    /// which is what makes the control scale-invariant (series law 7).
    void update_shift() {
        shift_bands_ = formant_shift_st_ / (12.0 * std::log2(ratio_));
    }

    /// Linear interpolation into the (possibly frozen) level array at a
    /// fractional index. Out of range reads as 0 rather than wrapping: wrapping
    /// would fold the bottom of the bank into the top, which is audible and
    /// wrong.
    double shifted_level(double x) const {
        const double floor_x = std::floor(x);
        const auto lower = static_cast<long long>(floor_x);
        const double frac = x - floor_x;
        const auto count = static_cast<long long>(active_);
        const double a = (lower >= 0 && lower < count) ? source_[static_cast<std::size_t>(lower)] : 0.0;
        const double b =
            (lower + 1 >= 0 && lower + 1 < count) ? source_[static_cast<std::size_t>(lower + 1)] : 0.0;
        return a + frac * (b - a);
    }

    /// Maintains the sliding crossing count in O(1): one bit leaves the window,
    /// one enters, and the running total tracks both.
    void update_zero_crossings(double x) {
        const bool sign = x >= 0.0;
        const bool crossing = sign != previous_sign_;
        previous_sign_ = sign;

        const std::size_t word = zcr_position_ >> 6;
        const std::uint64_t bit = std::uint64_t{1} << (zcr_position_ & 63u);
        if ((zcr_bits_[word] & bit) != 0u) --zcr_count_;
        if (crossing) {
            zcr_bits_[word] |= bit;
            ++zcr_count_;
        } else {
            zcr_bits_[word] &= ~bit;
        }
        if (++zcr_position_ >= zcr_length_) zcr_position_ = 0;

        zcr_hz_ = static_cast<double>(zcr_count_) * sample_rate_ /
                  static_cast<double>(zcr_length_);
    }

    /// The exponent that turns a 10→90 % time into a one-pole coefficient.
    /// `ln 9 = 2.19722…`; `BallisticsFilterT` ships the rounded 2.2 and this
    /// file matches it deliberately — a 0.13 % disagreement between two things
    /// both called a "10→90 % time" is exactly the kind of drift that makes a
    /// measured follower time fail a test for no real reason.
    static constexpr double kTenToNinety = 2.2;

    static constexpr double kPiConstant = 3.14159265358979323846;

    std::array<Band, kBandSlots> bands_{};
    std::array<double, kBandSlots> source_{};
    std::array<std::uint64_t, kZcrWords> zcr_bits_{};

    DcBlocker<double> modulator_dc_{};
    DcBlocker<double> carrier_dc_low_{};
    DcBlocker<double> carrier_dc_high_{};
    TptFilterT<double> sibilance_hp_{};
    osc::VaOscillator oscillator_{};
    VcaT<double> vca_{};
    Xorshift32 rng_{kNoiseSeed};

    double sample_rate_ = 48000.0;
    double freq_lo_ = 120.0;
    double freq_hi_ = 7000.0;
    double ratio_ = 1.0;
    double q_band_ = 1.0;
    double q_section_ = 1.0;

    double attack_ms_ = 1.5;
    double release_ms_ = 15.0;
    double noise_mix_ = 0.15;
    double unvoiced_sensitivity_ = 0.5;
    double sibilance_mix_ = 0.35;
    double formant_shift_st_ = 0.0;
    double shift_bands_ = 0.0;
    double output_trim_db_ = 0.0;
    double output_trim_ = 1.0;
    double dry_wet_ = 1.0;
    double pitch_hz_ = kDefaultPitchHz;
    double pulse_width_ = 0.5;

    double pre_emphasis_state_ = 0.0;
    double unvoiced_ = 0.0;
    double unvoiced_coefficient_ = 0.0;
    double zcr_hz_ = 0.0;

    std::size_t active_ = 16;
    std::size_t zcr_length_ = 1;
    std::size_t zcr_position_ = 0;
    std::size_t zcr_count_ = 0;

    int band_count_ = 16;
    CarrierSource carrier_source_ = CarrierSource::internal;
    InternalWave internal_wave_ = InternalWave::saw;
    bool previous_sign_ = true;
    bool unvoiced_latched_ = false;
    bool freeze_ = false;
    bool freeze_edge_ = false;
};

using Vocoder = VocoderT<float>;
using Vocoder64 = VocoderT<double>;

}  // namespace pulp::signal
