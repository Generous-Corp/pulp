#pragma once

/// @file tape_machine.hpp
/// A tape machine as an INSERT — record chain in, playback chain out, no delay
/// and no feedback anywhere in it.
///
/// The catalog already models tape as a thing that happens inside a loop: the
/// character delay's physical tier (`character_delay/tape_physical.hpp`) runs
/// Jiles-Atherton magnetization and Wallace playback loss once per recirculation
/// so colouration compounds per repeat. This block models the other half of what
/// a deck does — the part that is a *system* rather than a saturator — and calls
/// into that tier rather than restating any of its physics.
///
/// What is genuinely new here, and why a saturator cannot fake it:
///
/// **The EQ is standard-defined, not a tone knob.** Every recording standard
/// closes the raw flux-transfer gap with a PAIR of time constants — a bass
/// turnover and a treble turnover — realized as complementary record and
/// playback networks. NAB defines a bass shelf at every reel speed it covers;
/// the documented CCIR/IEC reel data has only the treble turnover. That single
/// structural difference is the whole "thin European tape" folklore: a tape
/// recorded IEC and replayed NAB diverges in the low end because one curve has a
/// bass network and the other does not. `set_eq_curve` is deliberately
/// independent of `set_archetype` so a Studer preset can be told to run NAB and
/// reproduce that mismatch on purpose.
///
/// **Bias is one control that moves three things at once.** AC bias linearizes
/// the hysteresis loop at the operating point, and pushing past the
/// manufacturer's optimum buys lower distortion at the cost of high-frequency
/// output while also erasing more of whatever has already printed through.
/// "Hotter bias, darker and cleaner, less pre-echo" is the entire practical
/// mental model, and it is three coupled consequences of one number — which is
/// exactly what a plain drive knob cannot express.
///
/// **Gap loss is an extinction, not a roll-off.** As the recorded wavelength
/// approaches the reproduce head's physical gap width, opposing flux polarities
/// inside the gap cancel: a `sinc` with a genuine null at `λ = g`, not a corner
/// frequency. It is modelled separately from the reused Wallace filter because
/// it is a different mechanism with its own published closed form.
///
/// **The compander is a real 2:1/1:2 pair.** dbx-family noise reduction halves
/// the programme's dynamic range in dB before it meets tape noise and doubles it
/// back afterwards, so noise that entered in between comes back attenuated in
/// proportion to how quiet the programme was. That is a level-dependent process;
/// no fixed EQ reproduces it.
///
/// ## Three decisions that are not obvious, and what they avoid
///
/// **The prototype network is normalized to unity at 1 kHz.** `§4.1`'s
/// reproduce prototype `H(s) = (1 + s·t₁)/(1 + s·t₂)` is stated unnormalized,
/// and unnormalized it is a +25.6 dB gain at 1 kHz and +36.1 dB asymptotically
/// for NAB (`t₁/t₂ = 3180/50`). Inserting that raw would mean the saturation
/// stage sees a 1 kHz tone 25.6 dB off its intended operating point — a bias
/// test measuring distortion at 1 kHz would be measuring the EQ instead. Every
/// published NAB/IEC/cassette curve is quoted in dB *relative to 1 kHz*, which
/// is also the tape-alignment reference frequency, so normalizing there is the
/// convention rather than a liberty. `kEqReferenceHz` fixes it once.
///
/// **`H` is the RECORD network; playback is its exact inverse.** The source
/// document says both things — its diagram and its rationale label the record
/// side "pre-emphasis" (highs pushed harder into the nonlinearity), while one
/// sentence calls the record side the reciprocal. They cannot both hold, because
/// `(1 + s·t₁)/(1 + s·t₂)` with `t₁ ≫ t₂` RISES with frequency. This header
/// resolves it toward pre-emphasis-on-record, for three reasons: it is the only
/// assignment under which the stated rationale is true; it matches the reused
/// tape tier's own internal emphasis pair; and it is what the physics says — the
/// resulting playback network is `−10.04 dB at 10 kHz relative to 1 kHz`, against
/// `−9.94 dB` for the fully-derived NAB reproduce characteristic (head
/// differentiation compensated). The other assignment gets that sign backwards.
///
/// **The playback network's degenerate pole is damped, not ignored.** When a
/// curve has no bass shelf (`t₁ = 0`: IEC/CCIR reel at every speed, and NAB at
/// 30 ips) the record network is a plain one-pole whose bilinear form carries a
/// zero at Nyquist. Its exact inverse therefore carries a POLE at Nyquist —
/// marginally stable, unbounded gain on whatever hiss and saturation product
/// lives up there. `kEqInversePoleDamping` pulls that pole just inside the unit
/// circle. The cost is measured across every degenerate case at three sample
/// rates rather than assumed: 0.0396 dB of worst-case round-trip ripple over
/// 20 Hz–15 kHz — seven times inside the ±0.3 dB flatness criterion — in
/// exchange for 14 dB less gain at Nyquist than the obvious lighter setting.
/// Damping is applied ONLY to the degenerate case; NAB's and cassette's
/// inverses are proper first-order sections and are left exact.
///
/// **The insert is aligned, the way a real machine is.** The reused Wallace
/// loss model is tuned as delay-loop character, where per-repeat darkening IS
/// the effect and absolute level is set by the feedback control. Dropped into an
/// insert unchanged it loses **19.7 dB at 1 kHz** on the cassette preset at mid
/// age, and a twenty-decibel hole at 1 kHz is not tape character, it is a broken
/// insert. So the loss the playback chain will apply at the alignment frequency
/// is computed in closed form from the filters actually in the path and undone —
/// which is precisely what setting reproduce gain against a calibration tape
/// does on real hardware. `reproduce_alignment_db()` reports it, and
/// `kAlignmentCeilingDb` bounds it.
///
/// ## Latency, stated plainly
///
/// This block does NOT report zero latency, and cannot. Two of its inherited
/// stages are constant delays:
///
///   * the reused hysteresis stage's 4× half-band oversampling wrap — a
///     linear-phase FIR pair, `(65−1)/2 + (65−1)/4 = 48` host samples,
///     rate-independent, and inherited unchanged along with that stage's
///     antialiasing policy;
///   * the wow/flutter read-position modulation, which is a delay-line read and
///     therefore needs a nominal offset at least as large as the deepest
///     modulation it must swing through. `kInstabilityNominalMs` fixes that
///     offset at a CONSTANT independent of archetype, curve, speed and age, so
///     the reported latency never moves under the audio thread.
///
/// Minimum-phase and IIR group delay (the gap-loss FIR, the EQ sections, the
/// Wallace tilt) is colouration and is NOT reported, the same convention the
/// rest of the catalog uses for a biquad. `latency_samples()` reports the sum of
/// the two constant delays, plus the print-through offset when pre-echo is
/// enabled, and the acceptance suite asserts it against a MEASURED impulse
/// position rather than trusting the arithmetic.
///
/// ## No feedback path
///
/// This is a pure insert: record chain, then playback chain, both feed-forward.
/// Forge's `worst_case_gain` field is therefore not applicable, and the catalog
/// header says so explicitly rather than omitting it. `worst_case_insertion_gain()`
/// exists anyway, as an asserted bound on the largest gain any single stage can
/// present, because "not applicable" is a worse answer than a number when a
/// reviewer is sizing headroom.
///
/// RT contract: `prepare(sample_rate)` may allocate — it sizes the print-through
/// line for the worst configured offset, the wow/flutter line for the worst
/// configured modulation depth, and designs one gap-loss FIR per legal speed.
/// `set_bias`, `set_drive`, `set_age`, `set_eq_curve`, `set_crosstalk_db`,
/// `set_companding_enabled`, `set_print_through`, `set_mix`, `process()` and
/// `reset()` never allocate, never lock, never perform I/O, and are safe on the
/// audio thread. `set_speed_ips` and `set_archetype` are the two exceptions and
/// are CONTROL-THREAD ONLY: the reused physical tier redesigns its own
/// speed-dependent minimum-phase loss FIR when the tape speed changes, which
/// allocates. That is inherited behaviour, it is asserted by the suite rather
/// than assumed, and this block pre-designs its OWN speed-dependent filters at
/// `prepare()` so nothing in this file contributes to it. All state is POD;
/// zero-init is a valid fresh instance, and `reset()` restores it.
///
/// References. Reproduce gap-loss `sinc` with its extinction null: W. K.
/// Westmijze, *Studies on Magnetic Recording*, Philips Research Reports 8, 1953.
/// Speed ladder, NAB/IEC-CCIR time constants at 30 ips, IEC/CCIR at 7.5/15 ips,
/// and the ≥40 dB 100 Hz–12 kHz crosstalk figure: Studer A800 multichannel
/// recorder technical data. Cassette equalization and the fixed 4.75 cm/s speed:
/// IEC 60094 cassette parts; Richard Hess, "Cassette Equalization: The 4 dB
/// Ambiguity at 16 kHz". NAB 3180/50 µs reel constants: documented NAB
/// reel-to-reel reproduce practice, corroborated in secondary literature —
/// **honest gap**, the exact NAB document title/year could not be reconfirmed
/// against a live source, so the constants are carried on corroboration rather
/// than on a citation this header would be over-claiming. dbx-family 2:1/1:2
/// linear-dB companding: published concept only, no trademarked curve and no
/// proprietary time constant. Print-through mechanism and the tails-out pre/post
/// echo asymmetry: general documented phenomenon. Bilinear transform: textbook
/// digital filter design. Jiles-Atherton hysteresis, Wallace loss, head bump,
/// wow/flutter and the age table are REUSED from `character_delay/`, which
/// carries their citations.

// `character_delay/tape_physical.hpp` MUST come before `tape_loss.hpp` here,
// which is why this block is not alphabetized. `hysteresis.hpp` (pulled in by
// tape_physical) names `detail::LinearPhaseOversamplingStage2x` unqualified from
// inside `pulp::signal::chardelay`, and that only resolves to
// `pulp::signal::detail` while `pulp::signal::chardelay::detail` does not exist
// yet. `tape_loss.hpp` opens `chardelay::detail`, so including it first makes the
// nearer namespace win and the build fails on a name that is plainly there.
#include <pulp/signal/tape_machine_components.hpp>
#include <pulp/signal/character_delay/tape_physical.hpp>

#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>
#include <pulp/signal/character_delay/tape.hpp>
#include <pulp/signal/character_delay/tape_loss.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// The tape machine insert.
template <typename SampleType = float>
class TapeMachineT {
public:
    using Curve = TapeCurve;
    using Archetype = TapeArchetype;

    // ── Design parameters (the complete roster) ───────────────────────────

    /// Bias `−1` (under) .. `0` (manufacturer optimum) .. `+1` (over).
    static constexpr double kBiasMin = -1.0;
    static constexpr double kBiasMax = 1.0;
    static constexpr double kBiasDefault = 0.0;

    /// Over-bias darkens; under-bias brightens. Both are documented DIRECTIONS
    /// with no citable closed-form transfer curve — commercial treatments reach
    /// for full Preisach/Stoner-Wohlfarth simulation, which is out of scope for
    /// an insert. **Honest gap**: these three constants encode the direction and
    /// rough magnitude of a documented trade, not any machine's measured curve.
    /// [design parameter] default 6 dB per unit, range 3–10.
    static constexpr double kOverBiasHfCutDb = 6.0;
    /// [design parameter] default 4 dB per unit, range 2–8.
    static constexpr double kUnderBiasHfBoostDb = 4.0;
    /// [design parameter] default 8000 Hz, range 5000–10000.
    static constexpr double kBiasShelfHz = 8000.0;

    /// A parabola in bias, centred on optimum: minimum distortion at 0, rising
    /// either side. Multiplies the drive fed to the reused hysteresis stage.
    /// [design parameter] default 0.6, range 0.3–1.0.
    static constexpr double kBiasDistK = 0.6;

    /// Over-bias erases more of whatever has already printed through — the
    /// documented direction. Subtracted from the print-through level.
    /// [design parameter] default 3 dB per unit, range 1–6.
    static constexpr double kBiasErasureDb = 3.0;

    /// Drive maps `[0, 1]` onto this dB span, which is then multiplied by the
    /// bias parabola. The floor is 0 dB so `drive = 0` leaves the hysteresis
    /// stage at unity input — the condition an EQ measurement needs.
    /// [design parameter] default span 0 .. +24 dB, range 12–36 dB at the top.
    static constexpr double kDriveDbMax = 24.0;
    static constexpr double kDriveDefault = 0.3;

    /// Crosstalk bounds are the union of the per-archetype ranges.
    static constexpr double kCrosstalkDbMin = -45.0;
    static constexpr double kCrosstalkDbMax = -22.0;

    /// Inter-channel coupling worsens with frequency (documented general
    /// behaviour of inductive/capacitive coupling), so the leak is tilted up
    /// above a corner and shelf-limited so it can never exceed
    /// `crosstalk_db + kCrosstalkTiltMaxDb` at any frequency.
    /// [design parameter] corner default 5000 Hz, range 3000–8000 Hz.
    static constexpr double kCrosstalkTiltHz = 5000.0;
    /// [design parameter] default +6 dB total, range 4–9 dB. The realized
    /// first-order shelf reaches +5.53 dB above its 1 kHz reference and rises at
    /// a measured ~1.9 dB/octave through its transition, inside the source
    /// spec's stated 2–6 dB/octave window at the shallow end.
    static constexpr double kCrosstalkTiltMaxDb = 6.0;

    /// Print-through level bounds. These ARE the age table's extremes, so a
    /// baked parameter cannot ask for a level the age axis cannot produce.
    static constexpr double kPrintThroughDbMin = -80.0;
    static constexpr double kPrintThroughDbMax = -38.0;

    /// Wrap offset between the printing layer and the printed one. The real
    /// offset depends on reel radius, tape thickness and wind position and
    /// genuinely varies through a reel; a single representative constant is an
    /// honest simplification, not a measured value.
    /// [design parameter] default 700 ms, range 200–2000 ms.
    static constexpr double kPrintThroughOffsetMsDefault = 700.0;
    static constexpr double kPrintThroughOffsetMsMin = 200.0;
    static constexpr double kPrintThroughOffsetMsMax = 2000.0;

    /// Pre-echo is documented as the weaker of the two in the tails-out storage
    /// convention professionals use. [design parameter] default 4 dB, range 2–8.
    static constexpr double kPreEchoAsymmetryDb = 4.0;

    /// Ceiling on the reproduce alignment gain, in dB. See
    /// `reproduce_alignment_db()` for what alignment is and why it exists; this
    /// is the guard that keeps a pathological configuration from turning the
    /// reused hiss generator into the loudest thing in the mix.
    /// [design parameter] default +24 dB, range 12–36 dB.
    static constexpr double kAlignmentCeilingDb = 24.0;

    /// Nominal read offset of the wow/flutter delay line, in ms. Must exceed the
    /// deepest modulation the reused age table can ask for — 1.2 ms of wow plus
    /// 0.4 ms × (1 + 0.5 + 0.25) of the flutter harmonic stack = 1.9 ms — and is
    /// held CONSTANT so `latency_samples()` never moves with archetype, curve,
    /// speed or age. [design parameter] default 2.0 ms, range 1.9–5.0 ms.
    static constexpr double kInstabilityNominalMs = 2.0;

    /// Group delay of the reused hysteresis stage's 4× half-band oversampling
    /// wrap, in host samples. Rate-independent, inherited, and reported.
    static int oversampler_latency_samples() noexcept {
        return chardelay::TapePhysicalChannel::oversampler_latency_samples();
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    TapeMachineT() { set_archetype(Archetype::ampex_350_440); }

    /// Sizes both delay lines for their worst configured case and designs one
    /// gap-loss FIR per legal speed, so nothing in this file allocates later.
    /// May allocate.
    void prepare(double sample_rate) {
        if (std::isfinite(sample_rate) && sample_rate > 0.0)
            sample_rate_ = sample_rate;

        for (int index = 0; index < 2; ++index)
            channels_[static_cast<std::size_t>(index)].prepare(sample_rate_, index);

        instability_nominal_samples_ = static_cast<int>(std::llround(
            units::ms_to_samples(kInstabilityNominalMs, sample_rate_)));
        print_max_samples_ = static_cast<int>(std::llround(
            units::ms_to_samples(kPrintThroughOffsetMsMax, sample_rate_)));

        compander_l_.prepare(sample_rate_);
        compander_r_.prepare(sample_rate_);

        update_speed(speed_ips_, /*redesign=*/true);
        update_eq();
        apply_age();
        update_crosstalk();
        update_print_through();
        reset();
    }

    /// Never allocates. A zero-initialised instance is already in this state
    /// apart from the constructor's archetype preset.
    void reset() {
        for (auto& channel : channels_) channel.reset();
        compander_l_.reset();
        compander_r_.reset();
    }

    // ── Parameters ────────────────────────────────────────────────────────

    /// Applies the full baked preset. CONTROL THREAD ONLY — see the header's RT
    /// contract; a speed change reaches the reused tier's FIR redesign.
    void set_archetype(Archetype archetype) {
        archetype_ = archetype;
        const tape::ArchetypePreset preset = tape::archetype_preset(archetype);
        curve_ = preset.default_curve;
        crosstalk_db_ = preset.crosstalk_db;
        companding_ = preset.companding;
        age01_ = preset.age01;
        set_speed_ips(preset.default_speed_ips);
        update_eq();
        apply_age();
        update_crosstalk();
        update_print_through();
    }

    Archetype archetype() const noexcept { return archetype_; }

    /// Snapped to the nearest legal speed for the active archetype. CONTROL
    /// THREAD ONLY — see the header's RT contract.
    void set_speed_ips(double ips) {
        if (!std::isfinite(ips)) return;
        update_speed(tape::snap_speed_ips(archetype_, ips), /*redesign=*/false);
        update_eq();
    }

    double speed_ips() const noexcept { return speed_ips_; }

    /// Independent of archetype on purpose: a Studer really could be told to run
    /// NAB, and the resulting low-end mismatch is the point.
    void set_eq_curve(Curve curve) {
        curve_ = curve;
        update_eq();
    }

    Curve eq_curve() const noexcept { return curve_; }

    void set_bias(float bias_c) {
        if (!std::isfinite(static_cast<double>(bias_c))) return;
        bias_request_ = std::clamp(static_cast<double>(bias_c), kBiasMin, kBiasMax);
        update_bias();
        update_print_through();
    }

    /// The bias actually in force: what was requested plus the age table's
    /// drift, clamped. Exposed so a caller can show the effective value rather
    /// than the requested one.
    double effective_bias() const noexcept { return bias_; }

    void set_drive(float drive01) {
        if (!std::isfinite(static_cast<double>(drive01))) return;
        drive01_ = std::clamp(static_cast<double>(drive01), 0.0, 1.0);
        update_bias();
    }

    double drive() const noexcept { return drive01_; }

    void set_age(float age01) {
        if (!std::isfinite(static_cast<double>(age01))) return;
        age01_ = std::clamp(static_cast<double>(age01), 0.0, 1.0);
        apply_age();
        update_print_through();
    }

    double age() const noexcept { return age01_; }

    void set_crosstalk_db(float db) {
        if (!std::isfinite(static_cast<double>(db))) return;
        crosstalk_db_ = std::clamp(static_cast<double>(db), kCrosstalkDbMin, kCrosstalkDbMax);
        update_crosstalk();
    }

    double crosstalk_db() const noexcept { return crosstalk_db_; }

    void set_companding_enabled(bool enabled) noexcept { companding_ = enabled; }
    bool companding_enabled() const noexcept { return companding_; }

    /// `level_db` is the post-echo level before bias erasure; `offset_ms` is the
    /// wrap offset; `pre_echo_enabled` adds the lookahead and the latency that
    /// goes with it.
    void set_print_through(float level_db, float offset_ms, bool pre_echo_enabled) {
        if (!std::isfinite(static_cast<double>(level_db)) ||
            !std::isfinite(static_cast<double>(offset_ms)))
            return;
        print_through_db_ =
            std::clamp(static_cast<double>(level_db), kPrintThroughDbMin, kPrintThroughDbMax);
        print_offset_ms_ = std::clamp(static_cast<double>(offset_ms),
                                      kPrintThroughOffsetMsMin, kPrintThroughOffsetMsMax);
        pre_echo_ = pre_echo_enabled;
        update_print_through();
    }

    double print_through_db() const noexcept { return print_through_db_; }
    double print_offset_ms() const noexcept { return print_offset_ms_; }
    bool pre_echo_enabled() const noexcept { return pre_echo_; }

    void set_mix(float wet01) noexcept {
        if (!std::isfinite(static_cast<double>(wet01))) return;
        mix_ = std::clamp(static_cast<double>(wet01), 0.0, 1.0);
    }

    // ── Audio ─────────────────────────────────────────────────────────────

    void process(const SampleType* in_l, const SampleType* in_r, SampleType* out_l,
                 SampleType* out_r, int n_frames) {
        for (int n = 0; n < n_frames; ++n) {
            const double dry_l = static_cast<double>(in_l[n]);
            const double dry_r = static_cast<double>(in_r[n]);

            // Inputs immediately enter several recursive stages. Treat a
            // non-finite frame as a recovery boundary before it can latch
            // those states; reset is allocation-free by contract.
            if (!std::isfinite(dry_l) || !std::isfinite(dry_r)) {
                recover_audio_fault();
                out_l[n] = SampleType{0};
                out_r[n] = SampleType{0};
                continue;
            }

            // Record chain. Companding first, so the compressor sees the
            // programme rather than the tape's own colouration.
            double left = companding_ ? compander_l_.encode(dry_l) : dry_l;
            double right = companding_ ? compander_r_.encode(dry_r) : dry_r;

            left = channels_[0].record(left, drive_gain_, inverse_drive_gain_);
            right = channels_[1].record(right, drive_gain_, inverse_drive_gain_);

            // Playback chain up to the point where the two channels meet.
            left = channels_[0].playback(left);
            right = channels_[1].playback(right);

            // Crosstalk: each channel leaks a tilted copy of the other. Read
            // both before writing either, or the second leak would be computed
            // from a channel that already contains the first.
            const double leak_l = crosstalk_gain_ * channels_[0].crosstalk_tilt.process(right);
            const double leak_r = crosstalk_gain_ * channels_[1].crosstalk_tilt.process(left);
            left += leak_l;
            right += leak_r;

            left = channels_[0].print_through(left, print_taps_);
            right = channels_[1].print_through(right, print_taps_);

            // Reproduce EQ, then the bias-dependent high shelf cascaded with it,
            // then the expander.
            left = channels_[0].bias_shelf.process(channels_[0].playback_eq.process(left));
            right = channels_[1].bias_shelf.process(channels_[1].playback_eq.process(right));

            if (companding_) {
                left = compander_l_.decode(left);
                right = compander_r_.decode(right);
            }

            const double output_l = dry_l + mix_ * (left - dry_l);
            const double output_r = dry_r + mix_ * (right - dry_r);
            if (!std::isfinite(output_l) || !std::isfinite(output_r)) {
                recover_audio_fault();
                out_l[n] = SampleType{0};
                out_r[n] = SampleType{0};
                continue;
            }
            out_l[n] = static_cast<SampleType>(output_l);
            out_r[n] = static_cast<SampleType>(output_r);
        }
    }

    /// Reported exactly, never estimated (series law 5). The sum of the two
    /// constant delays this block cannot avoid — the reused oversampling wrap
    /// and the wow/flutter line's nominal read offset — plus the print-through
    /// offset when pre-echo is enabled. Constant across archetype, curve, speed
    /// and age by construction.
    int latency_samples() const noexcept {
        return oversampler_latency_samples() + instability_nominal_samples_ +
               (pre_echo_ ? print_taps_.offset_samples : 0);
    }

    /// The largest gain any single stage in this insert can present, as a bound
    /// a reviewer can size headroom against.
    ///
    /// There is no feedback path here, so Forge's `worst_case_gain` field does
    /// not apply and the catalog row says so. This is the useful number
    /// instead, and it is measured on the REALIZED sections over the whole
    /// `(curve × frequency)` grid to Nyquist — not on the analog prototype,
    /// which is where an earlier version of this got it wrong by 8 dB: the
    /// prototype has no equivalent of the damped inverse's behaviour at
    /// Nyquist, so a bound derived from it is not a bound.
    ///
    /// It is a large number, and the reason is structural rather than a defect
    /// to tune away. A reproduce network for a curve with no bass shelf is
    /// `(1 + s·t₂)`, a differentiator, so its gain climbs 6 dB per octave to
    /// the top of the band and its ceiling therefore rises with sample rate.
    /// The programme never sees it: the record network is that section's exact
    /// reciprocal, so anything arriving at the reproduce network has already
    /// been attenuated by the same amount, and the pair is unity. What DOES see
    /// it is whatever was injected in between — the reused hiss generator,
    /// which is lowpassed at 4 kHz and so has almost nothing up there. The
    /// suite asserts the consequence directly by bounding the module's output
    /// on silence and on a full-scale input.
    double worst_case_insertion_gain() const noexcept {
        double worst = 1.0;
        const double nyquist = 0.5 * sample_rate_;
        for (const Curve curve : {Curve::nab, Curve::iec_ccir, Curve::cassette_type1,
                                  Curve::cassette_type2}) {
            const tape::EqTimeConstants tc = tape::eq_time_constants(curve, speed_ips_);
            TapeEqSection record, playback;
            record.set(tc, sample_rate_, /*inverse=*/false);
            playback.set(tc, sample_rate_, /*inverse=*/true);
            for (double hz = 20.0; hz < nyquist; hz *= 1.005) {
                worst = std::max(worst,
                                 units::db_to_linear(record.response_db(hz, sample_rate_)));
                worst = std::max(worst,
                                 units::db_to_linear(playback.response_db(hz, sample_rate_)));
            }
        }
        return worst * units::db_to_linear(kUnderBiasHfBoostDb);
    }

    // ── Test surface ──────────────────────────────────────────────────────
    //
    // The acceptance criteria for the EQ networks, the gap filter and the
    // compander are stated as tolerances on THOSE STAGES (±0.5 dB against an
    // analytic prototype, a ≥40 dB extinction null, ±0.1 dB of round-trip
    // transparency). None of them is measurable end-to-end through a chain that
    // also contains Wallace loss, a head bump, hiss and pitch modulation, so the
    // stages are reachable directly. See the suite's notes.

    const TapeEqSection& record_eq(int channel = 0) const noexcept {
        return channels_[static_cast<std::size_t>(channel)].record_eq;
    }

    const TapeEqSection& playback_eq(int channel = 0) const noexcept {
        return channels_[static_cast<std::size_t>(channel)].playback_eq;
    }

    /// Coefficients of this instance's reproduce gap-loss FIR at the active
    /// speed, so the suite can measure its response without rendering.
    const std::vector<double>& gap_fir() const noexcept {
        return channels_[0].gap_taps;
    }

    /// The reproduce gap width in force, in metres.
    double reproduce_gap_m() const noexcept {
        return tape::head_gap_geometry(speed_ips_).reproduce_m;
    }

    /// How much reproduce gain the machine is currently aligned with, in dB.
    ///
    /// A real deck is aligned by playing a calibration tape and setting the
    /// reproduce amplifier so the alignment tone reads 0 VU. Without doing the
    /// same thing here this insert would be unusable, and the number says why:
    /// the reused Wallace model at cassette speed and mid age loses **19.7 dB
    /// at 1 kHz** (spacing 12 µm at 4.7625 cm/s gives −13.7 dB, coating
    /// thickness another −6.0 dB). That model was tuned as delay-loop
    /// character, where per-repeat darkening IS the effect and absolute level is
    /// set by the feedback control. An insert has no such control, and a
    /// twenty-decibel hole at 1 kHz is not tape character — it is a broken
    /// insert.
    ///
    /// So the loss the playback chain will apply at `kEqReferenceHz` is computed
    /// from the SHIPPED filters actually in the path — the reused tier's fitted
    /// IIR tilt, its gap FIR, and this module's own reproduce-gap FIR — and
    /// undone. Not measured by rendering, and not a fudge factor: it is the
    /// closed-form magnitude of the three stages at one frequency.
    ///
    /// The gain sits AFTER the loss stage, so it lifts the reused hiss along
    /// with the programme. That is correct rather than unfortunate: a real
    /// reproduce amplifier amplifies head noise too, which is exactly why
    /// companding exists.
    double reproduce_alignment_db() const noexcept {
        return units::linear_to_db(channels_[0].alignment_gain);
    }

    double sample_rate() const noexcept { return sample_rate_; }

private:
    void recover_audio_fault() noexcept {
        for (auto& channel : channels_) channel.recover_audio_fault();
        compander_l_.reset();
        compander_r_.reset();
    }

    /// Where a print-through read lands, in samples. Two integers rather than a
    /// millisecond value so `process()` never converts.
    struct PrintTaps {
        int offset_samples = 0;
        double post_gain = 0.0;
        double pre_gain = 0.0;
        bool pre_echo = false;
    };

    /// Everything that is per-channel. Two of these; the only cross-channel
    /// coupling in the whole block is the crosstalk leak.
    struct Channel {
        chardelay::TapePhysicalChannel physical{};
        chardelay::TapeInstability instability{};
        chardelay::FractionalDelayLine instability_line{};
        chardelay::FixedFir gap_fir{};
        chardelay::FirstOrderShelf bias_shelf{};
        chardelay::FirstOrderShelf crosstalk_tilt{};
        TapeEqSection record_eq{};
        TapeEqSection playback_eq{};

        std::vector<double> gap_taps{};
        std::vector<float> print_line{};
        std::size_t print_write = 0;
        std::size_t print_valid = 0;

        double wow_depth_ms = 0.0;
        double flutter_depth_ms = 0.0;
        double nominal_samples = 0.0;
        double sample_rate = 48000.0;
        double alignment_gain = 1.0;

        /// Magnitude of an FIR at one frequency. Control-rate only.
        static double fir_magnitude(const std::vector<double>& taps, double hz,
                                    double fs) noexcept {
            if (taps.empty()) return 1.0;
            double re = 0.0, im = 0.0;
            for (std::size_t n = 0; n < taps.size(); ++n) {
                const double theta = 2.0 * chardelay::kPi * hz * static_cast<double>(n) / fs;
                re += taps[n] * std::cos(theta);
                im -= taps[n] * std::sin(theta);
            }
            return std::hypot(re, im);
        }

        /// Recompute the alignment gain from the filters actually in the path.
        void align(double ceiling_db) noexcept {
            const double tilt_db = chardelay::tape_loss_iir_magnitude_db(
                physical.loss_parameters(), tape::kEqReferenceHz);
            const double reused_gap =
                fir_magnitude(physical.gap_coefficients(), tape::kEqReferenceHz, sample_rate);
            const double own_gap = fir_magnitude(gap_taps, tape::kEqReferenceHz, sample_rate);
            const double loss = units::db_to_linear(tilt_db) *
                                std::max(reused_gap, 1e-6) * std::max(own_gap, 1e-6);
            alignment_gain = std::min(1.0 / std::max(loss, 1e-6),
                                      units::db_to_linear(ceiling_db));
        }

        void prepare(double fs, int index) {
            sample_rate = fs;
            physical.prepare(fs);
            instability.prepare(fs);
            // Distinct seeds per channel, DERIVED from the shared constant
            // rather than drawn, so the two channels decorrelate while both
            // renders stay bit-identical across a reset (series law 2). The
            // right channel also starts its flutter stack at the reused
            // fixed phase offset.
            const auto seed = static_cast<std::uint32_t>(
                chardelay::kPrngSeed ^ (0x9E3779B9u * static_cast<std::uint32_t>(index)));
            instability.configure(seed, index == 0 ? 0.0 : chardelay::kFlutterPhaseRight);
            physical.set_seeds(seed);

            nominal_samples = units::ms_to_samples(kInstabilityNominalMs, fs);
            const auto capacity = static_cast<std::size_t>(
                std::llround(nominal_samples + units::ms_to_samples(4.0, fs))) + 8u;
            instability_line.prepare(capacity);

            const auto print_capacity = static_cast<std::size_t>(std::llround(
                2.0 * units::ms_to_samples(kPrintThroughOffsetMsMax, fs))) + 4u;
            print_line.assign(print_capacity, 0.0f);
            print_write = 0;
            print_valid = 0;
        }

        void reset() {
            physical.reset();
            instability.reset();
            instability_line.reset();
            gap_fir.reset();
            bias_shelf.reset();
            crosstalk_tilt.reset();
            record_eq.reset();
            playback_eq.reset();
            std::fill(print_line.begin(), print_line.end(), 0.0f);
            print_write = 0;
            print_valid = 0;
        }

        void recover_audio_fault() noexcept {
            physical.recover_audio_fault();
            instability.reset();
            instability_line.discard_history();
            gap_fir.discard_history();
            bias_shelf.reset();
            crosstalk_tilt.reset();
            record_eq.reset();
            playback_eq.reset();
            print_write = 0;
            print_valid = 0;
        }

        void set_gap_taps(std::vector<double> taps) {
            gap_taps = std::move(taps);
            gap_fir.prepare(gap_taps.size());
        }

        /// Record EQ → bias-scaled drive → reused hysteresis → drive undone.
        ///
        /// The drive law is `f(g·x)/g`, the same construction `SaturatorT` uses:
        /// the reused stage already guarantees unity small-signal gain at its own
        /// boundary, and dividing by the same `g` that multiplied the argument
        /// keeps that guarantee true at every drive setting. Raising drive
        /// changes the colour, never the level of a quiet signal.
        double record(double x, double drive_gain, double inverse_drive_gain) noexcept {
            const double emphasized = record_eq.process(x);
            return physical.pre_process(emphasized * drive_gain) * inverse_drive_gain;
        }

        /// Reproduce gap loss → reused Wallace loss / head bump / hiss / chew →
        /// wow/flutter read-position modulation.
        double playback(double x) noexcept {
            gap_fir.push(x);
            const double gapped = gap_taps.empty()
                                      ? x
                                      : gap_fir.convolve(gap_taps.data(), gap_taps.size());
            const double lossy = physical.post_process(gapped) * alignment_gain;

            instability.tick();
            instability_line.push(lossy);
            const double offset_ms = instability.offset_ms(wow_depth_ms, flutter_depth_ms);
            const double read =
                nominal_samples + units::ms_to_samples(offset_ms, sample_rate);
            return instability_line.read(read);
        }

        /// Post-echo is always on and costs no latency: the tap is simply read
        /// later than the programme. Pre-echo delays the programme instead and
        /// reads the tap from where the programme has not arrived yet, which is
        /// why it is the mode that costs latency and why it is opt-in.
        double print_through(double x, const PrintTaps& taps) noexcept {
            const std::size_t size = print_line.size();
            if (size == 0 || taps.offset_samples <= 0) return x;
            const auto offset = static_cast<std::size_t>(taps.offset_samples);

            print_line[print_write] = static_cast<float>(x);
            const auto at = [&](std::size_t back) {
                if (back >= print_valid + 1u) return 0.0;
                return static_cast<double>(print_line[(print_write + size - back) % size]);
            };

            double y = 0.0;
            if (taps.pre_echo) {
                // Programme delayed by one offset; the print arrives at the head
                // of the stream (early) and again two offsets back (late).
                y = at(offset) + taps.pre_gain * at(0) + taps.post_gain * at(2u * offset);
            } else {
                y = at(0) + taps.post_gain * at(offset);
            }
            print_write = (print_write + 1u) % size;
            if (print_valid < size) ++print_valid;
            return y;
        }
    };

    void update_speed(double ips, bool redesign) {
        const bool changed = std::abs(ips - speed_ips_) > 1e-9;
        speed_ips_ = ips;
        if (!changed && !redesign) return;

        const tape::HeadGapGeometry geometry = tape::head_gap_geometry(speed_ips_);
        chardelay::TapeLossGeometry gap_geometry;
        gap_geometry.speed_ips = speed_ips_;
        gap_geometry.gap_m = geometry.reproduce_m;
        // The reused minimum-phase design helper, given THIS module's speed-class
        // reproduce gap rather than the delay tier's fixed 3 µm head. Same
        // construction (cepstral fold), a different filter.
        std::vector<double> taps = chardelay::design_tape_gap_fir(
            sample_rate_, chardelay::tape_gap_fir_taps(sample_rate_), gap_geometry);

        for (auto& channel : channels_) {
            channel.set_gap_taps(taps);
            channel.physical.set_speed_ips(speed_ips_);
            channel.align(kAlignmentCeilingDb);
        }
    }

    void update_eq() {
        const tape::EqTimeConstants tc = tape::eq_time_constants(curve_, speed_ips_);
        for (auto& channel : channels_) {
            channel.record_eq.set(tc, sample_rate_, /*inverse=*/false);
            channel.playback_eq.set(tc, sample_rate_, /*inverse=*/true);
        }
    }

    /// The age axis, applied everywhere it lands: the reused tier's own
    /// wow/flutter/spacing/hiss/chew/degrade columns, this module's two new
    /// columns (print-through level and bias drift), and the wow/flutter depths
    /// the instability generator reads.
    ///
    /// Age WRITES the print-through level, because that is what §4.7 means by
    /// "storage print grows with age". An explicit `set_print_through` after a
    /// `set_age` overrides it; the catalog node injects the baked parameter
    /// after the age parameter for exactly that reason.
    void apply_age() {
        for (auto& channel : channels_) {
            channel.physical.update(age01_);
            // Age moves the reused tier's spacing, so realignment belongs here
            // and at every speed change — the same two events that move it on a
            // real machine.
            channel.align(kAlignmentCeilingDb);
        }
        print_through_db_ = tape::age_print_through_db(age01_);
        update_bias();
    }

    void update_bias() {
        // The age table's drift is ADDED to what the user asked for, then
        // clamped — a tired machine biases hotter than its front panel says.
        bias_ = std::clamp(bias_request_ + tape::age_bias_drift(age01_), kBiasMin, kBiasMax);

        const double hf_shelf_db = -kOverBiasHfCutDb * std::max(0.0, bias_) +
                                   kUnderBiasHfBoostDb * std::max(0.0, -bias_);
        const double shelf_gain = units::db_to_linear(hf_shelf_db);
        for (auto& channel : channels_)
            channel.bias_shelf.set(kBiasShelfHz, shelf_gain, sample_rate_);

        const double distortion_mult = 1.0 + kBiasDistK * bias_ * bias_;
        drive_gain_ = units::db_to_linear(drive01_ * kDriveDbMax * distortion_mult);
        inverse_drive_gain_ = 1.0 / drive_gain_;

        // Wow and flutter depths come from the reused table, driven by this
        // module's own age axis.
        const double wow = chardelay::interpolate_knots(chardelay::kTapeAxis,
                                                        chardelay::kTapeWowDepthMs, age01_);
        const double flutter = chardelay::interpolate_knots(
            chardelay::kTapeAxis, chardelay::kTapeFlutterDepthMs, age01_);
        for (auto& channel : channels_) {
            channel.wow_depth_ms = wow;
            channel.flutter_depth_ms = flutter;
        }
    }

    void update_crosstalk() {
        const double tilt = units::db_to_linear(kCrosstalkTiltMaxDb);
        for (auto& channel : channels_)
            channel.crosstalk_tilt.set(kCrosstalkTiltHz, tilt, sample_rate_);
        // The tilt shelf is normalized to unity at the same 1 kHz reference the
        // EQ networks use, so a crosstalk measurement at 1 kHz reads the
        // configured figure EXACTLY rather than the figure plus half a decibel
        // of shelf skirt. Normalizing against the realized digital shelf, not
        // its analog prototype, so the bilinear's frequency warping does not
        // leak into that measurement. The stated ceiling becomes
        // `crosstalk_db + kCrosstalkTiltMaxDb − (skirt at 1 kHz)`, which is
        // strictly below the ceiling the spec asks for.
        crosstalk_gain_ = units::db_to_linear(
            crosstalk_db_ - tape::first_order_shelf_db(kCrosstalkTiltHz, tilt,
                                                       tape::kEqReferenceHz, sample_rate_));
    }

    void update_print_through() {
        print_taps_.offset_samples = std::min(
            static_cast<int>(std::llround(units::ms_to_samples(print_offset_ms_, sample_rate_))),
            print_max_samples_);
        const double reduction = kBiasErasureDb * std::max(0.0, bias_);
        const double post_db = print_through_db_ - reduction;
        print_taps_.post_gain = units::db_to_linear(post_db);
        print_taps_.pre_gain = units::db_to_linear(post_db - kPreEchoAsymmetryDb);
        print_taps_.pre_echo = pre_echo_;
    }

    std::array<Channel, 2> channels_{};
    TapeCompanderT<SampleType> compander_l_{};
    TapeCompanderT<SampleType> compander_r_{};

    double sample_rate_ = 48000.0;
    Archetype archetype_ = Archetype::ampex_350_440;
    Curve curve_ = Curve::nab;
    double speed_ips_ = 15.0;
    double bias_request_ = kBiasDefault;
    double bias_ = kBiasDefault;
    double drive01_ = kDriveDefault;
    double age01_ = 0.0;
    double crosstalk_db_ = -38.0;
    bool companding_ = false;
    double print_through_db_ = kPrintThroughDbMin;
    double print_offset_ms_ = kPrintThroughOffsetMsDefault;
    bool pre_echo_ = false;
    double mix_ = 1.0;

    double drive_gain_ = 1.0;
    double inverse_drive_gain_ = 1.0;
    double crosstalk_gain_ = 0.0;
    PrintTaps print_taps_{};
    int instability_nominal_samples_ = 0;
    int print_max_samples_ = 0;
};

using TapeMachine = TapeMachineT<float>;
using TapeMachine64 = TapeMachineT<double>;
using TapeCompander = TapeCompanderT<float>;

}  // namespace pulp::signal
