#pragma once

/// @file chorus_family.hpp
/// The chorus family — one engine, four documented voicings selected as data.
///
/// A chorus is a modulated short delay line mixed with the dry signal. What
/// separates a Boss CE-2 from a Juno ensemble from a Roland Dimension D from a
/// three-voice TriChorus is not the delay and not the interpolator: it is how
/// many taps there are, what phase relationship their modulators hold, and how
/// the taps are summed back into L and R. So the frame here — a fractional
/// delay read, an `LfoT` per voice, a dry/wet blend — is shared, and each
/// voicing is a row in a calibration table plus a branch in the mix matrix.
/// Nothing about a voicing's identity lives in a different code path through
/// the delay or the interpolation core, which is what makes the four auditable
/// against each other.
///
///   * **CE-2** — one mono BBD tap, triangle LFO, duplicated to L and R. The
///     reference guitar-chorus timbre.
///   * **Juno ensemble** — two taps, one per channel, LFOs an exact half cycle
///     apart, no cross-mix. Three fixed-rate modes (I / II / I+II).
///   * **Dimension D** — two inverted-phase taps like the Juno, *plus* the
///     complementary cross-mix: each channel's wet tap subtracted from the
///     opposite channel through a high-pass, with the dry bass-boosted to
///     replace what the cross-cancellation takes out. Width without warble.
///   * **TriChorus** — three sine-modulated voices at 120° spacing, routed
///     L / centre-split / R. No two voices are ever in phase, which is why it
///     is audibly denser than a two-tap ensemble.
///
/// ## Relationship to `chorus.hpp`
///
/// `pulp::signal::ChorusT` (chorus.hpp) is an earlier, thinner prototype: a
/// single sine LFO, a quadrature-offset stereo pair, depth expressed as a
/// fraction of the centre delay, and linear interpolation. It is not a subset
/// of this class — its depth law and its 90°-offset stereo image belong to no
/// documented hardware voicing — so this module ships **alongside** it rather
/// than extending it in place. `ChorusT` is public API (`signal.hpp` exports
/// it, `test/test_signal.cpp` covers it); removing it is a coordinated
/// public-API change, not something to fold into this module's landing. New
/// callers should use `ChorusEnsembleT`; `ChorusT` is superseded and should be
/// retired in a follow-up that also updates its exporter and its test.
///
/// ## Composition — what this module does NOT own
///
///   * **Modulation.** Every voice's modulator is a `pulp::signal::LfoT`, with
///     N-voice spacing expressed as `set_phase_offset(k / N)`. The 0.5-cycle
///     offset that gives the Juno and the Dimension D their inverted pair is
///     `LfoT`'s documented exact inversion for odd-symmetric shapes, asserted
///     in that toolkit's own suite. There is no LFO in this file.
///   * **Interpolation.** `Interpolator::lagrange` — 4-point (3rd-order)
///     Lagrange, per Laakso et al. 1996. There is no interpolation kernel in
///     this file, only the stencil fetch.
///   * **Ring storage.** `DelayLineT`, read at four integer offsets around the
///     fractional target.
///   * **BBD colour.** `chardelay::BbdChannel` — the clock-domain bandwidth law
///     and compander that the multi-character-delay module owns. See
///     `set_bbd_color` for the one substitution made and why.
///
/// ## Constants: published vs design parameter
///
/// Published, cited at the constant: the Juno's three-mode delay ranges and
/// LFO rates (pendragon-andyh's measured Juno-60 circuit analysis) and the
/// CE-2's practical 5–40 ms delay zone (ElectroSmash's open circuit analysis
/// of the MN3007/MN3101 pair). Everything else is a design parameter carrying a
/// default and a range: the CE-2's exact centre/depth/rate inside its cited
/// zone (Boss never published pot calibration curves), and the whole of the
/// Dimension D and TriChorus calibrations (no source documents either family at
/// that precision — an honest gap, not an omission).
///
/// ## Anti-aliasing policy (series law 4)
///
/// **The audio path is linear when `bbd_color` is off, so there is nothing to
/// oversample.** Three things could be mistaken for a nonlinearity:
///
///   1. The modulated read is a *resampling* operation, not a nonlinearity. Its
///      error is the Lagrange kernel's passband droop and imaging, which is the
///      cited design tradeoff (`kInterpOrder`), not an aliasing policy.
///   2. The trapezoid clamp (§ Dimension D) is a hard nonlinearity applied to
///      the **modulation signal**, at LFO rate. Its harmonics are multiples of
///      a sub-10 Hz fundamental and never enter the audio path except as delay
///      modulation. Oversampling it would be meaningless.
///   3. The one genuine audio-path nonlinearity, the BBD's asymmetric soft
///      clip, lives inside `chardelay::BbdChannel`, which already runs it at
///      `kBbdOversample`× behind a matched band-limiting pair. That policy is
///      inherited by composition and is deliberately not restated here.
///
/// ## Latency (series law 5)
///
/// `latency_samples()` is **0** for every voicing and every mode. The Lagrange
/// stencil spans `kInterpOrder + 1` samples around the fractional read
/// position, so it stays strictly behind the write pointer as long as the
/// minimum instantaneous delay stays `kGuardSamples` clear of it. Every shipped
/// calibration clears that guard by a factor of 19 or more — the tightest is
/// the Juno's 79.7 samples of minimum delay against a 4-sample guard at 48 kHz.
/// Enabling `bbd_color` only *adds* group delay (measured: 3 samples, from the
/// composed stage's band-limiting pair and its clocked write/read alignment),
/// never lookahead.
///
/// RT contract: `prepare()` allocates the two delay lines and is the only
/// function in this class that may. `set_*`, `process`, and `reset` allocate
/// nothing, take no locks, and perform no I/O. `set_voicing`/`set_juno_mode`
/// recompute filter and BBD coefficients, so they are control-thread calls;
/// they are allocation-free and safe to call from the audio thread if a host
/// insists. Every buffer is sized in `prepare()` for `kMaxDelayMs`, which
/// covers the widest retune the calibration tables' declared ranges allow, so
/// no `set_*` can outgrow the storage.

#include <pulp/signal/character_delay/bbd.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/interpolator.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// One chorus engine, four voicings. Stereo in place.
template <typename SampleType = float>
class ChorusEnsembleT {
public:
    enum class Voicing : std::uint8_t { ce2, juno_ensemble, dimension_d, tri_chorus };

    /// The Juno's three front-panel positions. The real unit's two mode
    /// switches are not mutually exclusive, so `mode_I_plus_II` engages both
    /// LFOs rather than selecting a third one.
    enum class JunoMode : std::uint8_t { mode_I, mode_II, mode_I_plus_II };

    // ── Structural constants ──────────────────────────────────────────────

    /// Most voices any voicing uses (TriChorus). Sizes the per-voice arrays.
    static constexpr int kMaxVoices = 3;
    static constexpr std::size_t kVoiceSlots = static_cast<std::size_t>(kMaxVoices);

    /// Lagrange interpolation order. [design parameter] default 3, range
    /// 1 (linear) .. 3. 3 is what ships; the guard below tracks it.
    static constexpr int kInterpOrder = 3;

    /// Samples of clearance the minimum instantaneous delay must keep from the
    /// write pointer so the `kInterpOrder + 1`-point stencil never reads ahead
    /// of it. [design parameter] default 4, range 2 .. 4 — tracks
    /// `kInterpOrder + 1`.
    static constexpr int kGuardSamples = kInterpOrder + 1;

    /// Storage ceiling, milliseconds. Not a musical parameter: it is sized so
    /// that *any* retune inside the calibration tables' declared ranges fits
    /// without reallocating (the widest is the CE-2's `d_center` 25 ms +
    /// `d_depth` 10 ms = 35 ms). [design parameter] default 40 ms, range
    /// 35 .. 200 ms.
    static constexpr double kMaxDelayMs = 40.0;

    // ── Dimension D constants ─────────────────────────────────────────────

    /// Trapezoid shaping gain: `trap = clamp(k · triangle, ±1)`. The dwell
    /// fraction at each modulation extreme is `1 − 1/k` of each half cycle.
    /// [design parameter] default 1.8 (≈44.4 % dwell), range 1.2 .. 3.0.
    static constexpr double kTrapK = 1.8;

    /// Corner of both the cross-feed high-pass and the complementary dry
    /// low-shelf. One constant, because the shelf exists precisely to restore
    /// what the high-passed cross-feed cancels — a mismatched pair would leave
    /// a hole or a bump at the crossover. [design parameter] default 200 Hz,
    /// range 100 .. 400 Hz. Honest gap: the SDD-320's actual corner is not
    /// published.
    static constexpr double kDimCornerHz = 200.0;

    /// Dry low-shelf gain below `kDimCornerHz`. [design parameter] default
    /// +1.5 dB, range 0.5 .. 3.0 dB. Honest gap, as above.
    static constexpr double kDimBoostDb = 1.5;

    // ── BBD colour constants ──────────────────────────────────────────────

    /// Position on `chardelay::kBbdAxis` used for the optional colour stage.
    /// [design parameter] default 1.0, range 0 .. 1. 1.0 selects the
    /// 1024-stage knot — the MN3007 family, which is the device the CE-2
    /// actually carries — along with that knot's drive and clock-jitter
    /// amounts.
    static constexpr double kBbdColorCharacter = 1.0;

    /// Milliseconds of the wet path left on the Lagrange line at the deepest
    /// modulation point when the BBD stage carries the rest. Keeps the line's
    /// read distance clear of `kGuardSamples` at every sample rate this module
    /// supports. [design parameter] default 0.5 ms (24 samples at 48 kHz),
    /// range 0.2 .. 2.0 ms.
    static constexpr double kBbdColorGuardMs = 0.5;

    // ── Calibration tables ────────────────────────────────────────────────

    /// One voicing's shipped calibration. `depth_ms` is the excursion at
    /// `set_depth(1)`, not a ceiling the depth macro sweeps towards: the guard
    /// analysis in this file's header, and the delay-range acceptance test,
    /// both read `d_center ± d_depth` as the full-depth extremes.
    struct Calibration {
        double center_ms;
        double depth_ms;
        double rate_hz;
        double mix_default;
        int voices;
        LfoWave wave;
        /// True when the taps read a mono sum of the input rather than their
        /// own channel — the CE-2 because the pedal is mono in and out, the
        /// TriChorus because its stereo field is *synthesised* by the L/C/R
        /// split and would otherwise depend on the source's own correlation.
        bool mono_source;
        /// True when the triangle is trapezoid-shaped before it modulates.
        bool trapezoid;
    };

    static constexpr Calibration calibration(Voicing v) {
        switch (v) {
            case Voicing::ce2:
                // d_center 12 ms [dp, range 5–25], d_depth 6 ms [dp, range
                // 2–10], rate 1.2 Hz [dp, range 0.05–10], internal blend 0.5
                // [dp, range 0.3–0.7 — the pedal has no mix knob]. The
                // enclosing 5–40 ms practical zone and the MN3007's
                // 5.12–51.2 ms device range are the cited part (ElectroSmash,
                // "Boss CE-2 Analysis"); the calibration inside it is not
                // published and is an honest gap.
                return {12.0, 6.0, 1.2, 0.5, 1, LfoWave::triangle, true, false};
            case Voicing::juno_ensemble:
                // Placeholder row: the Juno's numbers are per-mode and come
                // from juno_spec(). Only `voices`, `wave` and the routing flags
                // are read for this voicing.
                return {3.505, 1.845, 0.513, 0.5, 2, LfoWave::triangle, false, false};
            case Voicing::dimension_d:
                // All four numbers [dp] — honest gap. d_center 6 ms [range
                // 3–12], d_depth 1.5 ms [range 0.3–3] (deliberately shallow:
                // the unit is engineered not to sound like modulation), rate
                // 0.6 Hz [range 0.2–1.5], mix 0.5 [range 0.3–0.7]. Roland's
                // SDD-320 service notes document the topology, not constants.
                return {6.0, 1.5, 0.6, 0.5, 2, LfoWave::triangle, false, true};
            case Voicing::tri_chorus:
                // All [dp] — honest gap. The 120°-spaced three-voice L/C/R
                // topology is documented across the Dytronics Songbird
                // TSC-1380 lineage (Fractal Audio's Chorus block notes,
                // Eventide's TriceraChorus documentation); no source publishes
                // delay or rate figures, by design — the family spans many
                // implementations. d_center 15 ms [range 8–25], d_depth 4 ms
                // [range 1–8], rate 0.8 Hz [range 0.3–3], mix 0.5 [0.3–0.7].
                return {15.0, 4.0, 0.8, 0.5, 3, LfoWave::sine, true, false};
        }
        return {12.0, 6.0, 1.2, 0.5, 1, LfoWave::triangle, true, false};
    }

    /// The Juno's per-mode calibration. PUBLISHED, measured from the hardware:
    /// A. Hunt (pendragon-andyh), "Juno60/Chorus", github.com/pendragon-andyh/
    /// Juno60 — 1.66–5.35 ms at 0.513 Hz (mode I) and 0.863 Hz (mode II), and
    /// the narrower 3.3–3.7 ms window when both engage. Roland's own service
    /// nominals (~0.5 / ~0.83 Hz) corroborate the two rates.
    struct JunoModeSpec {
        double center_ms;
        double depth_ms;
        double rate_a_hz;
        double rate_b_hz;  ///< Second LFO; used only when `dual`.
        bool dual;
    };

    static constexpr JunoModeSpec juno_spec(JunoMode m) {
        constexpr double kLoMs = 1.66;   // published
        constexpr double kHiMs = 5.35;   // published
        constexpr double kRateI = 0.513;  // published, Hz
        constexpr double kRateII = 0.863; // published, Hz
        constexpr double kBothLoMs = 3.3; // published
        constexpr double kBothHiMs = 3.7; // published
        switch (m) {
            case JunoMode::mode_I:
                return {0.5 * (kLoMs + kHiMs), 0.5 * (kHiMs - kLoMs), kRateI, 0.0, false};
            case JunoMode::mode_II:
                return {0.5 * (kLoMs + kHiMs), 0.5 * (kHiMs - kLoMs), kRateII, 0.0, false};
            case JunoMode::mode_I_plus_II:
                return {0.5 * (kBothLoMs + kBothHiMs), 0.5 * (kBothHiMs - kBothLoMs),
                        kRateI, kRateII, true};
        }
        return {0.5 * (kLoMs + kHiMs), 0.5 * (kHiMs - kLoMs), kRateI, 0.0, false};
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    ChorusEnsembleT() { configure_voicing(); }

    /// Sizes both delay lines for `kMaxDelayMs` plus the interpolation stencil,
    /// prepares the composed primitives, and resets. The only allocating call.
    void prepare(double sample_rate) {
        sample_rate_ = (std::isfinite(sample_rate) && sample_rate > 0.0) ? sample_rate : 48000.0;

        const int capacity = static_cast<int>(
            std::ceil(kMaxDelayMs * 0.001 * sample_rate_)) + kGuardSamples + kInterpOrder + 2;
        for (auto& line : lines_) line.prepare(capacity);
        max_read_samples_ = static_cast<double>(capacity - kInterpOrder - 1);

        for (std::size_t k = 0; k < kVoiceSlots; ++k) {
            lfo_a_[k].prepare(sample_rate_);
            lfo_b_[k].prepare(sample_rate_);
            bbd_[k].prepare(sample_rate_);
            // Distinct clock-jitter streams per voice. Sharing one seed would
            // make three "independent" BBDs wander in lockstep, which is
            // exactly the correlation the third voice exists to avoid.
            bbd_[k].set_seed(chardelay::kPrngSeed ^ (0x9E3779B9u * static_cast<std::uint32_t>(k + 1)));
        }
        for (std::size_t c = 0; c < 2; ++c) {
            shelf_lowpass_[c].prepare(sample_rate_);
            shelf_lowpass_[c].set_cutoff(kDimCornerHz);
            cross_highpass_[c].prepare(sample_rate_);
            cross_highpass_[c].set_cutoff(kDimCornerHz);
        }

        prepared_ = true;
        configure_voicing();
        reset();
    }

    // ── Configuration ─────────────────────────────────────────────────────

    /// Switches the calibration table and rewinds every LFO phase. Adopts the
    /// new voicing's shipped rate: call `set_rate_hz` *after* `set_voicing`, not
    /// before, or the table default wins.
    void set_voicing(Voicing v) {
        voicing_ = v;
        configure_voicing();
    }

    Voicing voicing() const { return voicing_; }

    /// Ignored unless the voicing is `juno_ensemble`. Rewinds LFO phases, since
    /// the mode changes both rates and the delay window.
    void set_juno_mode(JunoMode m) {
        juno_mode_ = m;
        if (voicing_ == Voicing::juno_ensemble) configure_voicing();
    }

    JunoMode juno_mode() const { return juno_mode_; }

    /// LFO rate in Hz, 0.05 .. 10. **Ignored by `juno_ensemble`**, whose three
    /// modes run at their own measured fixed rates — that fixed-rate behaviour
    /// is the Juno sound, not an omission.
    void set_rate_hz(SampleType hz) {
        if (!std::isfinite(static_cast<double>(hz))) return;
        rate_hz_ = std::clamp(static_cast<double>(hz), kRateMinHz, kRateMaxHz);
        apply_rates();
    }

    double rate_hz() const { return rate_hz_; }

    /// Depth macro, 0 .. 1, over the voicing's `d_depth`. `set_depth(1)` is
    /// exactly the calibration table's excursion.
    void set_depth(SampleType depth01) {
        if (!std::isfinite(static_cast<double>(depth01))) return;
        depth_ = std::clamp(static_cast<double>(depth01), 0.0, 1.0);
    }

    /// Dry/wet, 0 .. 1. Note that the wet side is the voicing's **whole mix
    /// matrix**, which carries its own dry term (all four units blend inside
    /// the circuit). So `mix` crossfades bypass against the full circuit, not
    /// dry against wet-only, and the dry never disappears at `mix = 1`.
    void set_mix(SampleType mix01) {
        if (!std::isfinite(static_cast<double>(mix01))) return;
        mix_ = std::clamp(static_cast<double>(mix01), 0.0, 1.0);
    }

    /// 0 .. 1. Scales the Dimension D's cross-mix term and the TriChorus's
    /// centre-voice split. No effect on `ce2` (mono) or `juno_ensemble`, whose
    /// width is fixed by the inverted-phase topology itself.
    void set_stereo_width(SampleType width01) {
        if (!std::isfinite(static_cast<double>(width01))) return;
        width_ = std::clamp(static_cast<double>(width01), 0.0, 1.0);
    }

    /// Routes every voice's tap through `chardelay::BbdChannel` — the
    /// clock-domain bandwidth law and compander the multi-character-delay
    /// module owns.
    ///
    /// **Substitution, stated deliberately.** The spec asks for a
    /// `CharacterDelayT` configured to `Character::bbd`; this holds
    /// `BbdChannel` instead. `BbdChannel` *is* that character's device model —
    /// the composition is verbatim — while `CharacterDelayT` is the whole 2 s
    /// feedback delay engine wrapped around it, with reverse, tape and
    /// diffusion buffers costing megabytes per instance, three instances here,
    /// and no API to act as a pure colouring stage (its wet path is its own
    /// delay line, which would double every tap's delay).
    ///
    /// **The split.** The BBD is a clocked delay, so it cannot colour without
    /// delaying. It is given the fixed sub-delay
    /// `d_center − d_depth − kBbdColorGuardMs` and the Lagrange line carries
    /// the remainder *including all of the modulation*, so the total wet delay
    /// stays exactly on the calibration table and every delay, phase and
    /// latency property measured with the colour off holds with it on. Driving
    /// the BBD's clock from the instantaneous delay instead would be the more
    /// literal reading, but it buys nothing measurable: the bandwidth law
    /// `clamp(stages / t / 3, 300 Hz, 10 kHz)` sits on its 10 kHz ceiling at
    /// **every point of every voicing's sweep** (the slowest case is the CE-2
    /// at its 18 ms extreme: 1024 / 0.018 / 3 = 19.0 kHz, still nearly twice
    /// the ceiling), so instantaneous and centre evaluation are provably equal
    /// here, and a control-rate clock would only add a staircase to the pitch.
    void set_bbd_color(bool on) {
        bbd_color_ = on;
        update_bbd_stage();
    }

    bool bbd_color() const { return bbd_color_; }

    // ── Audio ─────────────────────────────────────────────────────────────

    void process(SampleType* left, SampleType* right, int n) {
        if (!prepared_ || left == nullptr || right == nullptr) return;

        const Calibration cal = calibration(voicing_);
        const bool juno = voicing_ == Voicing::juno_ensemble;
        const JunoModeSpec juno_cal = juno_spec(juno_mode_);
        const double center_ms = juno ? juno_cal.center_ms : cal.center_ms;
        const double depth_ms = depth_ * (juno ? juno_cal.depth_ms : cal.depth_ms);
        const double ms_to_samples = sample_rate_ * 0.001;
        const double boost_extra = units::db_to_linear(kDimBoostDb) - 1.0;

        for (int i = 0; i < n; ++i) {
            const double in_l = static_cast<double>(left[i]);
            const double in_r = static_cast<double>(right[i]);
            if (!std::isfinite(in_l) || !std::isfinite(in_r)) {
                discard_history();
                left[i] = right[i] = SampleType{0};
                continue;
            }
            const double src_l = cal.mono_source ? 0.5 * (in_l + in_r) : in_l;
            const double src_r = cal.mono_source ? src_l : in_r;

            lines_[0].push(static_cast<SampleType>(src_l));
            lines_[1].push(static_cast<SampleType>(src_r));

            std::array<double, kVoiceSlots> wet{};
            const auto active = static_cast<std::size_t>(voices_);
            for (std::size_t k = 0; k < active; ++k) {
                const double m = modulation(k);
                const double delay_ms = center_ms + depth_ms * m;
                delay_ms_[k] = delay_ms;

                double read_samples = delay_ms * ms_to_samples;
                if (bbd_color_) read_samples -= bbd_delay_samples_;
                read_samples = std::clamp(read_samples,
                                          static_cast<double>(kGuardSamples),
                                          max_read_samples_);

                double tap = read_lagrange(lines_[voice_line_[k]], read_samples);
                if (bbd_color_) tap = bbd_[k].process(tap);
                wet[k] = tap;
            }

            double matrix_l = 0.0;
            double matrix_r = 0.0;
            switch (voicing_) {
                case Voicing::ce2:
                    matrix_l = src_l + wet[0];
                    matrix_r = matrix_l;
                    break;
                case Voicing::juno_ensemble:
                    matrix_l = in_l + wet[0];
                    matrix_r = in_r + wet[1];
                    break;
                case Voicing::dimension_d: {
                    // Cross-feed is high-passed so only mid and high content
                    // participates in the cancellation; the dry is shelved by
                    // the complementary low-pass so the low end that the
                    // cross-mix would thin out stays solid and centred.
                    const double boost_l = in_l + boost_extra * shelf_lowpass_[0].process_lowpass(in_l);
                    const double boost_r = in_r + boost_extra * shelf_lowpass_[1].process_lowpass(in_r);
                    const double cross_l = cross_highpass_[0].process_highpass(wet[1]);
                    const double cross_r = cross_highpass_[1].process_highpass(wet[0]);
                    matrix_l = boost_l + wet[0] - width_ * cross_l;
                    matrix_r = boost_r + wet[1] - width_ * cross_r;
                    break;
                }
                case Voicing::tri_chorus:
                    matrix_l = in_l + wet[0] + width_ * 0.5 * wet[1];
                    matrix_r = in_r + wet[2] + width_ * 0.5 * wet[1];
                    break;
            }

            left[i] = static_cast<SampleType>((1.0 - mix_) * in_l + mix_ * matrix_l);
            right[i] = static_cast<SampleType>((1.0 - mix_) * in_r + mix_ * matrix_r);
        }
    }

    void reset() {
        for (auto& line : lines_) line.reset();
        for (std::size_t k = 0; k < kVoiceSlots; ++k) {
            lfo_a_[k].reset();
            lfo_b_[k].reset();
            bbd_[k].reset();
            delay_ms_[k] = 0.0;
        }
        for (std::size_t c = 0; c < 2; ++c) {
            shelf_lowpass_[c].reset();
            cross_highpass_[c].reset();
        }
    }

    /// Constant-time audio fault recovery. Public reset still physically clears
    /// storage; this path makes the old samples unreachable and resumes from
    /// logical silence without walking the prepared delay allocations.
    void discard_history() noexcept {
        for (auto& line : lines_) line.discard_history();
        for (std::size_t k = 0; k < kVoiceSlots; ++k) {
            lfo_a_[k].reset();
            lfo_b_[k].reset();
            bbd_[k].reset();
            delay_ms_[k] = 0.0;
        }
        for (std::size_t c = 0; c < 2; ++c) {
            shelf_lowpass_[c].reset();
            cross_highpass_[c].reset();
        }
    }

    /// 0 for every voicing. See this file's latency note.
    int latency_samples() const { return 0; }

    // ── Observability ─────────────────────────────────────────────────────
    //
    // These exist so the acceptance suite can measure the modulation directly
    // rather than only inferring it from audio. The suite still proves them
    // against the audio path (the delay-range test cross-checks
    // `current_delay_ms` against a click train tracked through `process`) —
    // an accessor that agreed with itself would test nothing.

    /// Instantaneous delay applied to voice `k` on the most recent sample, ms.
    double current_delay_ms(int voice) const {
        return (voice >= 0 && voice < kMaxVoices) ? delay_ms_[static_cast<std::size_t>(voice)] : 0.0;
    }

    int voice_count() const { return voices_; }

    /// Bandwidth the composed BBD stage is running at, Hz. Zero when the
    /// colour stage is off.
    double bbd_bandwidth_hz() const { return bbd_color_ ? bbd_[0].bandwidth_hz() : 0.0; }

    /// Delay the BBD colour stage carries when enabled, ms. Zero when off.
    double bbd_stage_delay_ms() const {
        return bbd_color_ ? bbd_delay_samples_ * 1000.0 / sample_rate_ : 0.0;
    }

    /// L1 norm of one modulated tap — the gain a fractional delay read can
    /// actually reach, as opposed to the unity a delay line suggests.
    ///
    /// A delay line's own L1 norm is 1, but the tap is a delay line read
    /// through the 4-point Lagrange kernel, and that kernel's coefficients are
    /// not all non-negative. At a half-sample offset they are
    /// `(−1/16, 9/16, 9/16, −1/16)`, whose absolute sum is `20/16 = 1.25` —
    /// the maximum over the fractional interval, and exactly attainable by an
    /// input whose signs match. Any bound that treats a modulated tap as unity
    /// gain is 25 % low; the acceptance suite recomputes this number directly
    /// from the shipped kernel rather than trusting the algebra.
    /// [derived constant, not a design parameter: it is a property of
    /// `kInterpOrder = 3`, and moving to linear interpolation would make it 1.]
    static constexpr double kTapL1 = 1.25;

    /// Peak output gain this configuration can produce for `|input| ≤ 1` at
    /// `mix = 1`, as an L1 (worst-case-over-all-inputs) bound. This module has
    /// no feedback path — every voicing is dry plus N feedforward taps — so the
    /// bound is a closed-form sum, and Forge's `worst_case_gain` cites it
    /// rather than estimating (series law 8).
    ///
    /// Two terms are larger than they look, which is why this is computed and
    /// not written down as `1 + N`:
    ///
    ///   * each tap carries `kTapL1 = 1.25`, not 1;
    ///   * the Dimension D's cross-feed high-pass carries
    ///     `2 / (1 + tan(π f_c / f_s))` — just under 2, not the unity its
    ///     *passband* gain suggests. Passband gain is a steady-state
    ///     sinusoidal figure; a peak bound needs the impulse response's L1
    ///     norm, and for a first-order high-pass those differ by a factor of
    ///     almost two. Its companion low-shelf is better behaved: a
    ///     non-negative impulse response makes that one's L1 norm exactly its
    ///     DC gain.
    ///
    /// The sample-rate dependence is real and comes from that high-pass term.
    double worst_case_gain() const {
        switch (voicing_) {
            case Voicing::ce2:
            case Voicing::juno_ensemble:
                return 1.0 + kTapL1;
            case Voicing::dimension_d:
                return shelf_l1() + kTapL1 + width_ * kTapL1 * highpass_l1();
            case Voicing::tri_chorus:
                return 1.0 + kTapL1 + width_ * 0.5 * kTapL1;
        }
        return 1.0 + kTapL1;
    }

    /// L1 norm of the dry low-shelf: its DC gain.
    double shelf_l1() const { return units::db_to_linear(kDimBoostDb); }

    /// L1 norm of the first-order cross-feed high-pass at the prepared rate.
    double highpass_l1() const {
        const double t = std::tan(kPi * kDimCornerHz / sample_rate_);
        return 2.0 / (1.0 + t);
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    /// Rate parameter bounds, Hz. [design parameter] default span 0.05 .. 10.
    static constexpr double kRateMinHz = 0.05;
    static constexpr double kRateMaxHz = 10.0;

    /// Reads the delay line at a fractional distance with the 4-point Lagrange
    /// stencil. Four integer reads around the target — the ring arithmetic and
    /// the kernel both belong to primitives this module composes, so all that
    /// lives here is the stencil layout: `y0` at `floor(d)`, `y1` one sample
    /// older, and the two outer nodes either side.
    static double read_lagrange(const DelayLineT<SampleType>& line, double delay_samples) {
        const int i = static_cast<int>(std::floor(delay_samples));
        const double frac = delay_samples - static_cast<double>(i);
        const double ym1 = static_cast<double>(line.read(i - 1));
        const double y0 = static_cast<double>(line.read(i));
        const double y1 = static_cast<double>(line.read(i + 1));
        const double y2 = static_cast<double>(line.read(i + 2));
        return Interpolator::lagrange(frac, ym1, y0, y1, y2);
    }

    /// Advances one voice's modulator(s) by a sample and returns `[-1, +1]`.
    double modulation(std::size_t index) {
        if (juno_dual_) {
            // Both LFOs engage simultaneously — the real unit's two mode
            // switches are not exclusive. The averaging-and-clamp law is a
            // [design parameter]: the analog mixer's summing headroom is not
            // published. The clamp is provably inactive for two bipolar
            // triangles (|0.5(a + b)| ≤ 1) and is kept as the belt-and-braces
            // the spec states, not as an active shaper.
            const double a = lfo_a_[index].next();
            const double b = lfo_b_[index].next();
            return std::clamp(0.5 * (a + b), -1.0, 1.0);
        }
        const double a = lfo_a_[index].next();
        if (!trapezoid_) return a;
        // Dwells at the modulation extremes instead of reversing sharply,
        // which is what lets the Dimension D's excursion be shallow enough to
        // read as width rather than as pitch modulation.
        return std::clamp(kTrapK * a, -1.0, 1.0);
    }

    void configure_voicing() {
        const Calibration cal = calibration(voicing_);
        const bool juno = voicing_ == Voicing::juno_ensemble;
        const JunoModeSpec juno_cal = juno_spec(juno_mode_);

        voices_ = cal.voices;
        trapezoid_ = cal.trapezoid;
        juno_dual_ = juno && juno_cal.dual;
        rate_hz_ = juno ? juno_cal.rate_a_hz : cal.rate_hz;

        // Voice → delay line. Mono-source voicings read one line; the
        // inverted-phase pairs read their own channel.
        for (std::size_t k = 0; k < kVoiceSlots; ++k)
            voice_line_[k] = (cal.mono_source || k == 0) ? 0 : 1;

        for (std::size_t k = 0; k < kVoiceSlots; ++k) {
            // Voice k of N sits at k/N cycles — the toolkit's N-voice spacing,
            // which for N = 2 is the exact inversion the Juno and Dimension D
            // are built on and for N = 3 is the TriChorus's 120°.
            const double offset = static_cast<double>(k) / static_cast<double>(voices_);
            lfo_a_[k].set_wave(cal.wave);
            lfo_a_[k].set_phase_offset(offset);
            lfo_b_[k].set_wave(cal.wave);
            lfo_b_[k].set_phase_offset(offset);
            lfo_a_[k].reset();
            lfo_b_[k].reset();
        }
        apply_rates();
        update_bbd_stage();
    }

    void apply_rates() {
        const bool juno = voicing_ == Voicing::juno_ensemble;
        const JunoModeSpec juno_cal = juno_spec(juno_mode_);
        const double rate_a = juno ? juno_cal.rate_a_hz : rate_hz_;
        const double rate_b = juno ? juno_cal.rate_b_hz : rate_hz_;
        for (std::size_t k = 0; k < kVoiceSlots; ++k) {
            lfo_a_[k].set_rate_hz(rate_a);
            lfo_b_[k].set_rate_hz(rate_b);
        }
    }

    void update_bbd_stage() {
        if (!prepared_) return;
        const Calibration cal = calibration(voicing_);
        const bool juno = voicing_ == Voicing::juno_ensemble;
        const JunoModeSpec juno_cal = juno_spec(juno_mode_);
        const double center_ms = juno ? juno_cal.center_ms : cal.center_ms;
        const double depth_ms = juno ? juno_cal.depth_ms : cal.depth_ms;

        const double stage_ms = std::max(center_ms - depth_ms - kBbdColorGuardMs, 0.05);
        bbd_delay_samples_ = stage_ms * sample_rate_ * 0.001;
        for (std::size_t k = 0; k < kVoiceSlots; ++k)
            bbd_[k].update(kBbdColorCharacter, stage_ms * 0.001);
    }

    std::array<DelayLineT<SampleType>, 2> lines_{};
    std::array<EffectLfoT<double>, kVoiceSlots> lfo_a_{};
    std::array<EffectLfoT<double>, kVoiceSlots> lfo_b_{};
    std::array<chardelay::BbdChannel, kVoiceSlots> bbd_{};
    std::array<TptFilterT<double>, 2> shelf_lowpass_{};
    std::array<TptFilterT<double>, 2> cross_highpass_{};

    std::array<double, kVoiceSlots> delay_ms_{};
    std::array<std::size_t, kVoiceSlots> voice_line_{};

    double sample_rate_ = 48000.0;
    double max_read_samples_ = 0.0;
    double bbd_delay_samples_ = 0.0;
    double rate_hz_ = 1.2;
    double depth_ = 0.5;
    double mix_ = 0.5;
    double width_ = 1.0;

    Voicing voicing_ = Voicing::ce2;
    JunoMode juno_mode_ = JunoMode::mode_I;
    int voices_ = 1;
    bool trapezoid_ = false;
    bool juno_dual_ = false;
    bool bbd_color_ = false;
    bool prepared_ = false;
};

using ChorusEnsemble = ChorusEnsembleT<float>;
using ChorusEnsemble64 = ChorusEnsembleT<double>;

}  // namespace pulp::signal
