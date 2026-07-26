#pragma once

/// @file leslie.hpp
/// The two mechanical pitch-modulators of the electric-organ world: a rotary
/// speaker cabinet and the Hammond console's internal scanner vibrato.
///
/// They wear similar clothes and are different physics, which is why they are
/// one file and two classes rather than one class with a mode switch:
///
/// - **`LeslieRotaryT`** is a real horn and a real bass baffle physically
///   SPINNING inside a wooden box. The rotation produces a genuine Doppler
///   pitch shift (the source moves toward and away from the mic), a tremolo
///   (the horn's beam sweeps past the mic), and a timbral sweep (a horn is
///   brighter on-axis). Two rotors at different rates and different inertias,
///   split by a crossover, recorded by a stereo pair, inside a reflective
///   cabinet.
/// - **`ScannerVibratoT`** has no moving air at all. A passive lumped LC delay
///   line is tapped along its length and a motor-driven rotating capacitor
///   sweeps a pickup across the taps, so the output delay ramps up and back
///   each revolution. A time-varying delay is a pitch shift. Mix the swept
///   output back with the dry organ and the comb interference is the Hammond
///   chorus — a flanger built from a transmission line and a commutator in
///   1949.
///
/// ## Why a Leslie never sounds like an LFO
///
/// Nothing in it is one clean sine, and each of those is a separate structural
/// fact rather than a voicing choice:
///
/// - **The two rotors run at deliberately different rates**, so their
///   modulations beat at the difference frequency — 1.0 Hz at the tremolo
///   defaults, 0.20 Hz at chorale. Locking them equal would remove the single
///   most identifying feature of the sound.
/// - **The two rotors have different INERTIA.** The horn is light and arrives
///   at a new speed in about a second; the drum is heavy and takes several, and
///   coasts down slower than it spins up. A speed change is therefore not a
///   ramp, it is two ramps of different lengths — the horn gets there first and
///   the drum smears in behind it. That asymmetry IS the gesture; see
///   `set_speed`.
/// - **One cosine drives three effects.** The facing angle sets the path length
///   (Doppler), the beam gain (tremolo) and the shelf gain (brightness), so
///   they are inseparably locked in the model exactly as they are in the
///   cabinet. Three independent LFOs would let them drift apart, and the sound
///   would come apart with them.
/// - **Two mics at different angles** see the horn face them at different
///   phases, which is where the ping-pong swirl comes from — not from a stereo
///   widener downstream.
///
/// ## Series law 1 does not apply here
///
/// Stated explicitly rather than omitted: **neither engine contains a feedback
/// path.** Both are strictly feedforward — modulated delay, parallel reflection
/// taps, an amplitude multiply, and a dry mix. There is no loop for a
/// small-signal gain to accumulate around, so there is nothing to
/// unity-compensate or bound as a loop gain. What the registry needs instead is
/// a CONSTRUCTIVE-SUM bound: the worst case is horn + drum + reflections + dry
/// all peaking together. That bound is measured by the suite's parameter sweep,
/// not estimated — see `kWorstCaseGain` on each class.
///
/// ## Oversampling: none, and that is a policy, not an omission
///
/// Series law 4 asks every module to state its policy. Neither engine contains
/// an aliasing nonlinearity: the amplitude and directivity multiplies are by
/// band-limited control signals, and the crossover, shelves and delay reads are
/// linear. There is no in-band alias source to suppress, so oversampling would
/// cost cycles and change nothing. The Leslie tube amp's growl IS a
/// nonlinearity and is deliberately out of scope — compose a saturator ahead of
/// this node, where it can carry its own oversampling policy.
///
/// ## RT contract
///
/// `prepare()` sizes the delay lines and may allocate. `set_*`, `process()`,
/// `process_block()` and `reset()` never allocate, never lock, and never
/// perform I/O. All buffers are sized in `prepare()` from the MAXIMUM of every
/// parameter's range, so no later `set_*` can ask for a read the buffer cannot
/// serve. State is POD; zero-init is a valid stopped cabinet. The only
/// randomness is the optional seeded mechanical drift, rewound by `reset()` and
/// never automatable (series law 2).
///
/// References: Smith, Serafin, Abel & Berners, "Doppler Simulation and the
/// Leslie", Proc. DAFx-02, Hamburg, 2002 — the Doppler-via-modulated-delay
/// method, the rotating-source geometry, the two-delay-line stereo mic model,
/// the angle-dependent directivity filtering, and the first-reflection filters.
/// The speed of sound is the standard dry-air figure from the ideal-gas
/// approximation `c ≈ 331.3 + 0.606·T(°C)` m/s at 20 °C. Rotor speeds, inertia
/// times, the crossover corner, the scanner rate and the line-box tap topology
/// are documented hardware BEHAVIOUR implemented as named design parameters —
/// see the honest-gap notes on each constant; no service-manual numbers are
/// reproduced.

#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/interpolator.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/linkwitz_riley.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/slew_limiter.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// The three positions of the cabinet's speed switch.
enum class LeslieSpeed : std::uint8_t {
    stop,      ///< Brake. Both rotors ramp to rest and the modulation freezes.
    chorale,   ///< Slow. A wide, gentle chorus.
    tremolo,   ///< Fast. The shimmering swirl.
};

/// Acoustic constants shared by both engines in this file.
namespace leslie_detail {

/// Speed of sound in dry air at 20 °C, m/s. From the ideal-gas approximation
/// `c ≈ 331.3 + 0.606·T(°C)`: `331.3 + 0.606·20 ≈ 343.4`, rounded to 343.
///
/// A compile-time constant rather than a parameter on purpose: `c` varies by
/// ±0.6 m/s per °C, so a 10 °C room swing moves the Doppler depth by ±1.7 %,
/// far below the resolution at which radius and rate are actually voiced.
inline constexpr double kSpeedOfSound = 343.0;

inline constexpr double kTwoPi = 6.283185307179586476925286766559;

/// Extra samples kept beyond the longest modelled read so the 4-point
/// interpolator's `index - 1` and `index + 2` neighbours are always inside the
/// buffer. Four covers the stencil with a sample to spare.
/// [design parameter] default 4, range 2 .. 8 samples.
inline constexpr int kInterpolatorMargin = 4;

}  // namespace leslie_detail

// ═══════════════════════════════════════════════════════════════════════════
//  Leslie rotary speaker
// ═══════════════════════════════════════════════════════════════════════════

/// A rotary speaker cabinet: two rotors, a crossover, a stereo mic pair and a
/// reflective box. Mono or stereo in, always stereo out.
template <typename SampleType = float>
class LeslieRotaryT {
public:
    using Speed = LeslieSpeed;

    // ── Rotor rates ───────────────────────────────────────────────────────
    // Honest-gap note, applying to this whole block: that the horn runs faster
    // than the drum, and that both have a slow and a fast setting, is
    // documented Leslie behaviour. The exact rates are engineering defaults
    // calibrated to the documented RPM ranges, not values lifted from a
    // verified primary source — which is why every one of them is a parameter.

    /// [design parameter] horn chorale rate 0.80 Hz (≈48 RPM), range 0.6 .. 1.0.
    static constexpr double kHornSlowHz = 0.80;
    /// [design parameter] horn tremolo rate 6.70 Hz (≈400 RPM), range 5.5 .. 7.5.
    static constexpr double kHornFastHz = 6.70;
    /// [design parameter] drum chorale rate 0.60 Hz (≈36 RPM), range 0.5 .. 0.9.
    static constexpr double kDrumSlowHz = 0.60;
    /// [design parameter] drum tremolo rate 5.70 Hz (≈342 RPM), range 4.5 .. 6.5.
    static constexpr double kDrumFastHz = 5.70;

    // ── Inertia ───────────────────────────────────────────────────────────

    /// [design parameter] horn spin-up 1.0 s, range 0.3 .. 3.0 s.
    static constexpr double kHornAccelS = 1.0;
    /// [design parameter] horn spin-down 1.0 s, range 0.3 .. 3.0 s.
    static constexpr double kHornDecelS = 1.0;
    /// [design parameter] drum spin-up 3.0 s, range 1.0 .. 8.0 s.
    static constexpr double kDrumAccelS = 3.0;
    /// [design parameter] drum spin-down 5.0 s, range 1.0 .. 12.0 s. Longer
    /// than its spin-up because a heavy drum coasts.
    static constexpr double kDrumDecelS = 5.0;

    /// How much of a speed change the quoted accel/decel time covers.
    ///
    /// The inertia ramp is a one-pole, which approaches its target
    /// asymptotically and therefore has no finite "arrival" — but "the horn
    /// spins up in about a second" is a statement about arrival, not about a
    /// time constant. This constant is the bridge: the shipped seconds are the
    /// time to close this fraction of the distance, and the one-pole τ handed
    /// to `SlewLimiterT` is `seconds / ln(1/(1−fraction))`. At 0.95 that
    /// divisor is almost exactly 3, so a 3 s drum accel is a 1 s τ.
    ///
    /// Reading the seconds as τ directly instead would make the drum take
    /// three times as long to arrive as its own datasheet-style figure claims,
    /// which is the kind of quiet factor-of-three that only shows up when
    /// someone compares against a real cabinet.
    /// [design parameter] default 0.95, range 0.90 .. 0.99.
    static constexpr double kSettleFraction = 0.95;

    // ── Geometry ──────────────────────────────────────────────────────────

    /// [design parameter] horn rotation radius 0.18 m, range 0.10 .. 0.25 m.
    static constexpr double kHornRadiusM = 0.18;
    /// [design parameter] drum rotation radius 0.12 m, range 0.08 .. 0.18 m.
    /// Smaller than the horn's, which is why the low end wobbles more gently
    /// than the top shimmers.
    static constexpr double kDrumRadiusM = 0.12;
    /// [design parameter] mic distance 1.0 m, range 0.3 .. 3.0 m. Trades
    /// Doppler against tremolo: a closer mic sweeps through more of the beam.
    static constexpr double kMicDistanceM = 1.0;
    /// [design parameter] mic pair included angle 45°, range 0 .. 90°. 0 is
    /// mono-ish (both mics see the same facing phase), 90 is wide ping-pong.
    static constexpr double kMicAngleDeg = 45.0;

    /// Constant offset added to every modelled delay so the read pointer stays
    /// strictly inside the buffer even at the rotor phase where the path length
    /// is at its minimum.
    /// [design parameter] default 0.5 ms, range 0.2 .. 1.0 ms.
    static constexpr double kDBiasMs = 0.5;

    // ── Band split, beam and brightness ───────────────────────────────────

    /// [design parameter] crossover 800 Hz, range 700 .. 900 Hz. The documented
    /// Leslie crossover network is around 800 Hz; the exact value is a
    /// constant, not a citation.
    static constexpr double kCrossoverHz = 800.0;

    /// [design parameter] tremolo depth 0.5, range 0 .. 0.9. Gain runs from
    /// `1 − depth` facing away to 1 facing the mic, so it is bounded at or
    /// below unity and cannot build.
    static constexpr double kAmDepth = 0.5;

    /// [design parameter] horn brightness sweep 6 dB, range 0 .. 12 dB.
    static constexpr double kDirDepthDb = 6.0;
    /// [design parameter] drum brightness sweep 2 dB, range 0 .. 6 dB. Weaker
    /// than the horn's — a bass baffle is far less directional.
    static constexpr double kDrumDirDepthDb = 2.0;
    /// [design parameter] directivity shelf corner 2 kHz, range 1 .. 4 kHz.
    static constexpr double kDirCornerHz = 2000.0;

    // ── Cabinet ───────────────────────────────────────────────────────────

    /// [design parameter] 2 reflection taps, range 1 .. 4.
    static constexpr int kNumReflections = 2;
    /// [design parameter] first reflection 3.5 ms, range 2.5 .. 6.0 ms.
    static constexpr double kReflDelayMs = 3.5;
    /// [design parameter] inter-tap spacing 1.5 ms, range 1.0 .. 3.0 ms.
    static constexpr double kReflSpacingMs = 1.5;
    /// [design parameter] reflection low-pass 3 kHz, range 1 .. 8 kHz.
    static constexpr double kReflCornerHz = 3000.0;
    /// [design parameter] reflection level −12 dB, range −60 .. −6 dB.
    static constexpr double kReflLevelDb = -12.0;
    /// At or below this the reflections are switched off EXACTLY rather than
    /// left at a vanishing gain, so "reflections off" is a state a test can
    /// assert against rather than a very small number.
    /// [design parameter] default −60 dB, range −80 .. −40 dB.
    static constexpr double kReflectionOffDb = -60.0;

    // ── Drift ─────────────────────────────────────────────────────────────

    /// [design parameter] motor wow 3 cents (1σ) of rate, range 0 .. 10 cents.
    /// 0 disables the walk entirely. This is the "not perfectly regulated
    /// motor" life; it is off unless asked for, because a default-on stochastic
    /// process makes every other test's expectation a distribution.
    static constexpr double kDriftCents = 3.0;
    /// [design parameter] drift correlation time 4 s, range 0.5 .. 30 s. Slow
    /// enough to read as wow rather than as noise on the rate.
    static constexpr double kDriftCorrelationS = 4.0;
    /// Fixed seed installed by `reset()`. A construction choice, never a
    /// parameter (series law 2).
    static constexpr std::uint32_t kDefaultSeed = 0x1e511e00u;

    // ── Parameter-range maxima, used for buffer sizing ────────────────────
    // `prepare` sizes from these rather than from the current settings, so no
    // later `set_*` can ask for a read the buffer cannot serve (series law 6).

    static constexpr double kMaxRadiusM = 0.25;
    static constexpr double kMaxDBiasMs = 1.0;
    static constexpr double kMaxReflDelayMs = 6.0;
    static constexpr double kMaxReflSpacingMs = 3.0;
    static constexpr int kMaxReflections = 4;

    /// The measured constructive-sum bound, in linear gain.
    ///
    /// There is no feedback path, so this is not a loop gain — it is the worst
    /// case of horn + drum + reflections + dry all peaking together, which for
    /// a feedforward chain is a finite static number. The suite's parameter
    /// sweep measures the actual maximum across the whole space and asserts it
    /// stays under this; the Forge registry cites the sweep, not this ceiling.
    ///
    /// The measured maximum is 5.12 (+14.2 dB), and most of it is one term: the
    /// directivity shelf is a BOOST, running from 0 dB off-axis up to
    /// `dir_depth_db` on-axis, so at that parameter's 12 dB maximum the shelf
    /// alone contributes a factor of four before horn, drum, reflections and
    /// dry are summed. Anyone budgeting this number from the topology alone
    /// will underestimate it badly unless they count that shelf.
    /// [design parameter] default 6.0 (+15.6 dB), range 2 .. 12.
    static constexpr double kWorstCaseGain = 6.0;

    LeslieRotaryT() {
        // The rotor phase accumulators are shared `LfoT`s; their SHAPE output is
        // discarded (see `run`), so the cheapest shape is chosen to be thrown
        // away rather than the default sine's transcendental.
        horn_phase_.set_wave(LfoWave::saw_up);
        drum_phase_.set_wave(LfoWave::saw_up);
        update();
    }

    /// Sizes the delay lines and filter state. May allocate.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        const int samples = worst_case_delay_samples(sample_rate_);
        horn_line_.prepare(samples);
        drum_line_.prepare(samples);
        horn_phase_.prepare(sample_rate_);
        drum_phase_.prepare(sample_rate_);
        horn_inertia_.prepare(sample_rate_);
        drum_inertia_.prepare(sample_rate_);
        horn_drift_.prepare(sample_rate_);
        drum_drift_.prepare(sample_rate_);
        for (auto& f : horn_shelf_) f.prepare(static_cast<SampleType>(sample_rate_));
        for (auto& f : drum_shelf_) f.prepare(static_cast<SampleType>(sample_rate_));
        horn_refl_lp_.prepare(static_cast<SampleType>(sample_rate_));
        drum_refl_lp_.prepare(static_cast<SampleType>(sample_rate_));
        // The blocker only has to clear subsonic offset; its corner sits below
        // the lowest rotor rate so it cannot eat the modulation itself.
        blocker_.set_pole(static_cast<SampleType>(
            1.0 - kDcCornerHz * leslie_detail::kTwoPi / sample_rate_));
        update();
        reset();
    }

    /// Returns to a fresh, silent cabinet already turning at the selected
    /// speed.
    ///
    /// The rotors are snapped to their target rate rather than started from
    /// rest, because `reset()` means "no history", and a spin-up ramp is
    /// history — one that would contaminate the first several seconds of every
    /// render made from a fresh state. Inertia is exercised by CHANGING speed,
    /// which is where it belongs and where the suite measures it.
    void reset() {
        horn_line_.reset();
        drum_line_.reset();
        horn_phase_.reset();
        drum_phase_.reset();
        horn_inertia_.set_immediate(target_horn_hz_);
        drum_inertia_.set_immediate(target_drum_hz_);
        horn_drift_.set_seed(kDefaultSeed);
        drum_drift_.set_seed(kDefaultSeed ^ 0x9e3779b9u);
        horn_drift_.reset();
        drum_drift_.reset();
        for (auto& f : horn_shelf_) f.reset();
        for (auto& f : drum_shelf_) f.reset();
        horn_refl_lp_.reset();
        drum_refl_lp_.reset();
        crossover_.reset();
        blocker_.reset();
    }

    // ── Controls ──────────────────────────────────────────────────────────

    /// The switch, and the most important control on the cabinet.
    ///
    /// Automate THIS, not the individual rate parameters. The whole character
    /// lives in the transition: because the horn's inertia is shorter than the
    /// drum's, a change to tremolo arrives as a bright shimmer first with the
    /// low end smearing in behind it over the next couple of seconds, and a
    /// change back coasts down with the drum trailing furthest. Jumping the
    /// rates directly skips the effect entirely.
    void set_speed(Speed speed) {
        speed_ = speed;
        update();
    }

    Speed speed() const { return speed_; }

    void set_horn_fast_hz(double hz) {
        horn_fast_hz_ = std::clamp(hz, 0.0, 20.0);
        update();
    }
    void set_horn_slow_hz(double hz) {
        horn_slow_hz_ = std::clamp(hz, 0.0, 20.0);
        update();
    }
    void set_drum_fast_hz(double hz) {
        drum_fast_hz_ = std::clamp(hz, 0.0, 20.0);
        update();
    }
    void set_drum_slow_hz(double hz) {
        drum_slow_hz_ = std::clamp(hz, 0.0, 20.0);
        update();
    }

    /// Seconds for the horn to close `kSettleFraction` of a speed change.
    void set_horn_accel_s(double seconds) {
        horn_accel_s_ = std::max(seconds, 0.0);
        update();
    }
    void set_horn_decel_s(double seconds) {
        horn_decel_s_ = std::max(seconds, 0.0);
        update();
    }
    void set_drum_accel_s(double seconds) {
        drum_accel_s_ = std::max(seconds, 0.0);
        update();
    }
    void set_drum_decel_s(double seconds) {
        drum_decel_s_ = std::max(seconds, 0.0);
        update();
    }

    void set_crossover_hz(double hz) {
        crossover_hz_ = std::clamp(hz, 20.0, 20000.0);
        update();
    }

    /// Doppler depth lives here, not in a depth knob: the peak fractional pitch
    /// shift is `r·ω/c`, so the SAME radius gives an order of magnitude
    /// different depth at chorale and at tremolo purely from the rate. That is
    /// why the two speeds are voiced by speed alone and never by re-tuning a
    /// depth curve (series law 7 — one scale-invariant shape, not two fitted
    /// ones).
    void set_horn_radius_m(double metres) {
        horn_radius_m_ = std::clamp(metres, 0.0, kMaxRadiusM);
    }
    void set_drum_radius_m(double metres) {
        drum_radius_m_ = std::clamp(metres, 0.0, kMaxRadiusM);
    }

    /// Mic distance trades Doppler against tremolo. A close mic sweeps through
    /// more of the beam, so it reads as more tremolo and relatively less pitch
    /// movement; a far mic is the reverse. "Subtle vintage" is a smaller radius
    /// with a farther mic; "in your face" is a larger radius up close.
    void set_mic_distance_m(double metres) {
        mic_distance_m_ = std::clamp(metres, kMinMicDistanceM, 10.0);
    }

    /// Stereo width IS the mic angle: 0° puts both mics on the same facing
    /// phase and collapses the image, 90° puts them in quadrature for the
    /// widest ping-pong. The tasteful default reads as two mics on the cabinet,
    /// which is exactly the classic recording setup.
    void set_mic_angle_deg(double degrees) {
        mic_angle_deg_ = std::clamp(degrees, 0.0, 180.0);
        update();
    }

    void set_am_depth(double depth) { am_depth_ = std::clamp(depth, 0.0, 0.9); }

    void set_dir_depth_db(double db) { dir_depth_db_ = std::clamp(db, 0.0, 24.0); }
    void set_drum_dir_depth_db(double db) { drum_dir_depth_db_ = std::clamp(db, 0.0, 24.0); }

    void set_dir_corner_hz(double hz) {
        dir_corner_hz_ = std::clamp(hz, 20.0, 20000.0);
        update();
    }

    void set_d_bias_ms(double ms) {
        d_bias_ms_ = std::clamp(ms, 0.0, kMaxDBiasMs);
    }

    /// The "box". Up around −8 dB the short cabinet taps comb the horn into the
    /// boxy resonance a close mic on a real cabinet picks up; at the −12 dB
    /// default it is a hint of room. A 3.5 ms first reflection cancels the
    /// direct signal at `1/(2·0.0035) ≈ 143 Hz` and its odd multiples, which is
    /// where that boxiness actually comes from.
    void set_reflection_db(double db) {
        reflection_db_ = std::clamp(db, -120.0, 0.0);
        update();
    }

    void set_num_reflections(int taps) {
        num_reflections_ = std::clamp(taps, 0, kMaxReflections);
        update();
    }
    void set_refl_delay_ms(double ms) {
        refl_delay_ms_ = std::clamp(ms, 0.0, kMaxReflDelayMs);
        update();
    }
    void set_refl_spacing_ms(double ms) {
        refl_spacing_ms_ = std::clamp(ms, 0.0, kMaxReflSpacingMs);
        update();
    }
    void set_refl_corner_hz(double hz) {
        refl_corner_hz_ = std::clamp(hz, 20.0, 20000.0);
        update();
    }

    /// Motor wow, in cents of rate (1σ). 0 disables the walk.
    void set_drift_cents(double cents) {
        drift_cents_ = std::clamp(cents, 0.0, 50.0);
        update();
    }

    /// Construction or preset choice only, never automated (series law 2).
    void set_seed(std::uint32_t seed) {
        horn_drift_.set_seed(seed);
        drum_drift_.set_seed(seed ^ 0x9e3779b9u);
    }

    void set_mix(double mix01) { mix_ = std::clamp(mix01, 0.0, 1.0); }

    // ── Observables ───────────────────────────────────────────────────────

    /// The rotors' CURRENT rates, mid-ramp included. Exposed because the
    /// inertia asymmetry is the module's headline behaviour and a meter that
    /// shows the two rotors converging at different times is worth having.
    double horn_rate_hz() const { return horn_inertia_.value(); }
    double drum_rate_hz() const { return drum_inertia_.value(); }

    /// The rates the switch is currently asking for.
    double target_horn_hz() const { return target_horn_hz_; }
    double target_drum_hz() const { return target_drum_hz_; }

    /// Rotor phases in cycles, `[0, 1)`.
    double horn_phase() const { return horn_phase_.phase(); }
    double drum_phase() const { return drum_phase_.phase(); }

    /// The facing phase of each mic, in cycles either side of the cabinet axis.
    double mic_face_offset() const { return mic_face_offset_; }

    /// The delay a rotor presents at a given phase and mic, in seconds — the
    /// modelled acoustic path, so a test can predict the Doppler rather than
    /// only observe it.
    double horn_delay_seconds(double phase_cycles, bool right_mic) const {
        return rotor_delay_seconds(phase_cycles, horn_radius_m_, right_mic);
    }
    double drum_delay_seconds(double phase_cycles, bool right_mic) const {
        return rotor_delay_seconds(phase_cycles, drum_radius_m_, right_mic);
    }

    /// Zero, and honestly so: every delay in this model is a modelled acoustic
    /// path, not processing latency to be compensated. Reporting the horn's
    /// bias delay as latency would have a host pull the whole cabinet earlier
    /// in time, which is not what a cabinet does.
    static constexpr int latency_samples() { return 0; }

    // ── Processing ────────────────────────────────────────────────────────

    void process(SampleType in_left, SampleType in_right, SampleType& out_left,
                 SampleType& out_right) {
        // One cabinet has one input. The dry path stays stereo so `mix` blends
        // against what actually arrived rather than against a folded-down copy.
        const double mono = 0.5 * (static_cast<double>(in_left) + static_cast<double>(in_right));
        double wet_l = 0.0;
        double wet_r = 0.0;
        run(mono, wet_l, wet_r);
        out_left = static_cast<SampleType>(snap_to_zero(
            mix_ * wet_l + (1.0 - mix_) * static_cast<double>(in_left)));
        out_right = static_cast<SampleType>(snap_to_zero(
            mix_ * wet_r + (1.0 - mix_) * static_cast<double>(in_right)));
    }

    void process(SampleType mono_in, SampleType& out_left, SampleType& out_right) {
        process(mono_in, mono_in, out_left, out_right);
    }

    void process_block(const SampleType* in_left, const SampleType* in_right,
                       SampleType* out_left, SampleType* out_right, int n) {
        for (int i = 0; i < n; ++i) process(in_left[i], in_right[i], out_left[i], out_right[i]);
    }

    void process_block(const SampleType* mono_in, SampleType* out_left, SampleType* out_right,
                       int n) {
        for (int i = 0; i < n; ++i) process(mono_in[i], out_left[i], out_right[i]);
    }

    /// The longest read any legal parameter set can produce, in samples.
    /// Public so the buffer-sizing test can check the shipped sizing against
    /// the declared ranges rather than against the current settings.
    static int worst_case_delay_samples(double sample_rate) {
        // The rotor path spans `L_max − L_min = (d+r) − (d−r) = 2r` — the mic
        // distance cancels, so the worst case is at MAXIMUM radius and is
        // independent of how far away the mic is.
        const double doppler_ms =
            2.0 * kMaxRadiusM / leslie_detail::kSpeedOfSound * 1000.0 + kMaxDBiasMs;
        const double reflection_ms =
            kMaxReflDelayMs + static_cast<double>(kMaxReflections - 1) * kMaxReflSpacingMs;
        const double longest_ms = std::max(doppler_ms, reflection_ms);
        return static_cast<int>(std::ceil(longest_ms * 0.001 * sample_rate)) +
               leslie_detail::kInterpolatorMargin;
    }

private:
    /// DC-blocker corner. Below the slowest rotor rate by a wide margin, so the
    /// blocker cannot eat the modulation it sits in front of.
    /// [design parameter] default 5 Hz, range 1 .. 20 Hz.
    static constexpr double kDcCornerHz = 5.0;

    /// Floor on the mic distance. It exists so the mic can never be inside the
    /// rotor circle, where `L_min = |d − r|` would stop being `d − r` and the
    /// geometry would fold. The floor exceeds the largest radius in range.
    /// [design parameter] default 0.3 m, range 0.26 .. 1.0 m.
    static constexpr double kMinMicDistanceM = 0.3;

    /// One rotor's modelled source-to-mic delay, in seconds.
    ///
    /// `L(θ) = sqrt(d² + r² − 2·d·r·cos(2π·(θ − θ_face)))` is the law of
    /// cosines on the triangle from the cabinet axis to the rotating source to
    /// the mic. Subtracting `L_min = d − r` makes the delay zero at the closest
    /// point, so the constant part of the path is not paid for in buffer
    /// length; the bias offset then keeps the read strictly inside the buffer.
    double rotor_delay_seconds(double phase_cycles, double radius_m, bool right_mic) const {
        const double face = right_mic ? mic_face_offset_ : -mic_face_offset_;
        const double c = std::cos(leslie_detail::kTwoPi * (phase_cycles - face));
        return path_delay_seconds(c, radius_m);
    }

    double path_delay_seconds(double cos_face, double radius_m) const {
        const double d = mic_distance_m_;
        const double length =
            std::sqrt(d * d + radius_m * radius_m - 2.0 * d * radius_m * cos_face);
        return (length - (d - radius_m)) / leslie_detail::kSpeedOfSound + d_bias_ms_ * 0.001;
    }

    /// A cubic-Lagrange read at a fractional delay in samples.
    ///
    /// The delay line's own `read` is linear, which puts a quantisation
    /// staircase on a swept read; the shared 4-point Lagrange kernel is what
    /// the rest of the catalog's modulated delays use, so the same one is used
    /// here rather than a second interpolation primitive. Weights are computed
    /// in double even for a float sample type: the fractional position IS the
    /// modulation, and rounding it to float would quantise the thing the effect
    /// exists to produce.
    static double read_cubic(const DelayLineT<SampleType>& line, double delay_samples) {
        const int index = static_cast<int>(std::floor(delay_samples));
        const double frac = delay_samples - static_cast<double>(index);
        return Interpolator::lagrange(frac,
                                      static_cast<double>(line.read(index - 1)),
                                      static_cast<double>(line.read(index)),
                                      static_cast<double>(line.read(index + 1)),
                                      static_cast<double>(line.read(index + 2)));
    }

    /// One rotor into one mic: Doppler, beam gain and brightness, all from the
    /// SAME cosine of the facing angle.
    ///
    /// Sharing the cosine is not a micro-optimisation, it is the model. In the
    /// cabinet the three effects are one geometric fact — where the horn is
    /// pointing — so computing them from one angle keeps them locked in phase
    /// by construction. Three separate modulators would be three things that
    /// can drift out of alignment.
    double rotor_into_mic(const DelayLineT<SampleType>& line, double phase_cycles,
                          double radius_m, double face, double dir_depth_db,
                          TptFilterT<SampleType>& shelf) const {
        const double cos_face = std::cos(leslie_detail::kTwoPi * (phase_cycles - face));

        const double delay_samples = path_delay_seconds(cos_face, radius_m) * sample_rate_;
        double value = read_cubic(line, delay_samples);

        // Beam gain: unity when the horn faces this mic, `1 − depth` when it
        // faces away. Bounded at or below 1 and outside any loop, so there is
        // nothing for it to build into.
        value *= 1.0 - am_depth_ * 0.5 * (1.0 - cos_face);

        // Brightness: a high shelf whose boost tracks the same facing angle,
        // full on-axis and flat off-axis.
        const double shelf_gain =
            units::db_to_linear(dir_depth_db * 0.5 * (1.0 + cos_face));
        const auto bands = shelf.process(static_cast<SampleType>(value));
        return static_cast<double>(bands.lowpass) +
               shelf_gain * static_cast<double>(bands.highpass);
    }

    /// The cabinet's first reflections off one rotor's line.
    ///
    /// The taps are summed and then low-passed once rather than low-passed
    /// individually. That is not an approximation: the filter is linear, so
    /// filtering the sum and summing the filtered taps are the same signal, and
    /// one filter is one filter's worth of state to keep coherent.
    double reflections(const DelayLineT<SampleType>& line, TptFilterT<SampleType>& lp) const {
        if (reflection_gain_ <= 0.0 || num_reflections_ <= 0) return 0.0;
        double sum = 0.0;
        for (int k = 0; k < num_reflections_; ++k) {
            sum += static_cast<double>(line.read(reflection_taps_[k]));
        }
        return reflection_gain_ * static_cast<double>(
                                      lp.process_lowpass(static_cast<SampleType>(sum)));
    }

    void run(double mono_in, double& out_left, double& out_right) {
        const double x = static_cast<double>(blocker_.process(static_cast<SampleType>(mono_in)));

        // ── Control rate: inertia, then drift, then phase ──
        double horn_hz = horn_inertia_.process(target_horn_hz_);
        double drum_hz = drum_inertia_.process(target_drum_hz_);
        if (drift_cents_ > 0.0) {
            horn_hz *= static_cast<double>(horn_drift_.next());
            drum_hz *= static_cast<double>(drum_drift_.next());
        }
        // The phase accumulator is the shared LFO, driven at the inertia-ramped
        // rate. Its shape output is discarded — what is wanted is the phase,
        // which two mics then read at two different facing angles, so no single
        // shape value could serve. `saw_up` is chosen because it is the
        // cheapest shape to evaluate and throw away.
        horn_phase_.set_rate_hz(horn_hz);
        drum_phase_.set_rate_hz(drum_hz);
        (void)horn_phase_.next();
        (void)drum_phase_.next();
        const double theta_h = horn_phase_.phase();
        const double theta_d = drum_phase_.phase();

        // ── Band split ──
        const auto split = crossover_.process(static_cast<SampleType>(x));
        horn_line_.push(split.high);
        drum_line_.push(split.low);

        // ── Direct paths, two mics ──
        const double horn_l = rotor_into_mic(horn_line_, theta_h, horn_radius_m_,
                                             -mic_face_offset_, dir_depth_db_, horn_shelf_[0]);
        const double horn_r = rotor_into_mic(horn_line_, theta_h, horn_radius_m_,
                                             +mic_face_offset_, dir_depth_db_, horn_shelf_[1]);
        const double drum_l = rotor_into_mic(drum_line_, theta_d, drum_radius_m_,
                                             -mic_face_offset_, drum_dir_depth_db_,
                                             drum_shelf_[0]);
        const double drum_r = rotor_into_mic(drum_line_, theta_d, drum_radius_m_,
                                             +mic_face_offset_, drum_dir_depth_db_,
                                             drum_shelf_[1]);

        // ── Cabinet ──
        // The reflection taps are fixed, so both mics receive the same
        // reflected component — which is physically right (the box radiates the
        // same reflections in both directions) and is also what keeps the
        // stereo image coming from the rotor geometry rather than from a
        // decorrelated reverb.
        const double refl = reflections(horn_line_, horn_refl_lp_) +
                            reflections(drum_line_, drum_refl_lp_);

        out_left = snap_to_zero(horn_l + drum_l + refl);
        out_right = snap_to_zero(horn_r + drum_r + refl);
    }

    void update() {
        switch (speed_) {
            case Speed::stop:
                target_horn_hz_ = 0.0;
                target_drum_hz_ = 0.0;
                break;
            case Speed::chorale:
                target_horn_hz_ = horn_slow_hz_;
                target_drum_hz_ = drum_slow_hz_;
                break;
            case Speed::tremolo:
                target_horn_hz_ = horn_fast_hz_;
                target_drum_hz_ = drum_fast_hz_;
                break;
        }

        // Exponential mode is the one mechanical inertia actually has: a
        // constant TIME CONSTANT, so a bigger speed change takes proportionally
        // longer. Linear mode would cover any distance in the same wall-clock
        // time, which is a portamento law, not a flywheel.
        horn_inertia_.set_mode(SlewMode::exponential);
        drum_inertia_.set_mode(SlewMode::exponential);
        horn_inertia_.set_rise_ms(tau_ms(horn_accel_s_));
        horn_inertia_.set_fall_ms(tau_ms(horn_decel_s_));
        drum_inertia_.set_rise_ms(tau_ms(drum_accel_s_));
        drum_inertia_.set_fall_ms(tau_ms(drum_decel_s_));

        crossover_.set_frequency(static_cast<SampleType>(crossover_hz_),
                                 static_cast<SampleType>(sample_rate_));

        for (auto& f : horn_shelf_) f.set_cutoff(static_cast<SampleType>(dir_corner_hz_));
        for (auto& f : drum_shelf_) f.set_cutoff(static_cast<SampleType>(dir_corner_hz_));
        horn_refl_lp_.set_cutoff(static_cast<SampleType>(refl_corner_hz_));
        drum_refl_lp_.set_cutoff(static_cast<SampleType>(refl_corner_hz_));

        // Half the included angle either side of the axis: the two mics are
        // `mic_angle_deg` apart in facing phase, which is the whole of the
        // stereo image.
        mic_face_offset_ = mic_angle_deg_ / 720.0;

        reflection_gain_ = reflection_db_ <= kReflectionOffDb
                               ? 0.0
                               : units::db_to_linear(reflection_db_);
        for (int k = 0; k < kMaxReflections; ++k) {
            const double ms = refl_delay_ms_ + static_cast<double>(k) * refl_spacing_ms_;
            reflection_taps_[k] = static_cast<int>(std::lround(ms * 0.001 * sample_rate_));
        }

        // Cents of rate wander converted to the fractional depth `DriftT`
        // speaks: a ratio of 1 + depth, so the percentage is the ratio's excess
        // over unity.
        const double ratio = units::cents_to_ratio(drift_cents_);
        horn_drift_.set_depth_percent((ratio - 1.0) * 100.0);
        drum_drift_.set_depth_percent((ratio - 1.0) * 100.0);
        horn_drift_.set_correlation_time(kDriftCorrelationS);
        drum_drift_.set_correlation_time(kDriftCorrelationS);
    }

    /// Converts a "time to arrive" into the one-pole time constant the slew
    /// limiter consumes. See `kSettleFraction`.
    static double tau_ms(double seconds) {
        constexpr double divisor_guard = 1e-9;
        const double divisor = std::max(-std::log(1.0 - kSettleFraction), divisor_guard);
        return seconds * 1000.0 / divisor;
    }

    double sample_rate_ = 48000.0;

    Speed speed_ = Speed::tremolo;
    double horn_fast_hz_ = kHornFastHz;
    double horn_slow_hz_ = kHornSlowHz;
    double drum_fast_hz_ = kDrumFastHz;
    double drum_slow_hz_ = kDrumSlowHz;
    double horn_accel_s_ = kHornAccelS;
    double horn_decel_s_ = kHornDecelS;
    double drum_accel_s_ = kDrumAccelS;
    double drum_decel_s_ = kDrumDecelS;
    double target_horn_hz_ = kHornFastHz;
    double target_drum_hz_ = kDrumFastHz;

    double crossover_hz_ = kCrossoverHz;
    double horn_radius_m_ = kHornRadiusM;
    double drum_radius_m_ = kDrumRadiusM;
    double mic_distance_m_ = kMicDistanceM;
    double mic_angle_deg_ = kMicAngleDeg;
    double mic_face_offset_ = kMicAngleDeg / 720.0;
    double d_bias_ms_ = kDBiasMs;

    double am_depth_ = kAmDepth;
    double dir_depth_db_ = kDirDepthDb;
    double drum_dir_depth_db_ = kDrumDirDepthDb;
    double dir_corner_hz_ = kDirCornerHz;

    int num_reflections_ = kNumReflections;
    double refl_delay_ms_ = kReflDelayMs;
    double refl_spacing_ms_ = kReflSpacingMs;
    double refl_corner_hz_ = kReflCornerHz;
    double reflection_db_ = kReflLevelDb;
    double reflection_gain_ = 0.0;
    int reflection_taps_[kMaxReflections] = {0, 0, 0, 0};

    double drift_cents_ = 0.0;
    double mix_ = 1.0;

    DelayLineT<SampleType> horn_line_{};
    DelayLineT<SampleType> drum_line_{};
    LfoT<double> horn_phase_{};
    LfoT<double> drum_phase_{};
    SlewLimiterT<double> horn_inertia_{};
    SlewLimiterT<double> drum_inertia_{};
    DriftT<double> horn_drift_{};
    DriftT<double> drum_drift_{};
    LinkwitzRileyT<SampleType> crossover_{};
    TptFilterT<SampleType> horn_shelf_[2]{};
    TptFilterT<SampleType> drum_shelf_[2]{};
    TptFilterT<SampleType> horn_refl_lp_{};
    TptFilterT<SampleType> drum_refl_lp_{};
    DcBlocker<SampleType> blocker_{};
};

using LeslieRotary = LeslieRotaryT<float>;
using LeslieRotary64 = LeslieRotaryT<double>;

// ═══════════════════════════════════════════════════════════════════════════
//  Hammond scanner vibrato / chorus
// ═══════════════════════════════════════════════════════════════════════════

/// The console's six-position vibrato/chorus switch, plus off.
enum class ScannerMode : std::uint8_t {
    off,  ///< Bit-exact dry passthrough.
    v1,   ///< Shallow vibrato — scanner output only.
    v2,   ///< Medium vibrato.
    v3,   ///< Full vibrato.
    c1,   ///< Shallow chorus — scanner plus dry.
    c2,   ///< Medium chorus.
    c3,   ///< Full chorus. The lush one.
};

/// The motor-scanned tapped delay line.
template <typename SampleType = float>
class ScannerVibratoT {
public:
    using Mode = ScannerMode;

    /// [design parameter] scanner rate 6.9 Hz, range 6.0 .. 7.5 Hz.
    ///
    /// Honest-gap note: the documented Hammond vibrato sweeps at roughly 7 Hz
    /// and the scanner is geared off the synchronous motor, so the rate is
    /// mains-locked rather than free. The exact figure is model- and
    /// mains-dependent and was not pinned to a verified primary source, so this
    /// is a calibration constant, not a citation.
    static constexpr double kScanHz = 6.9;

    /// [design parameter] the 50 Hz-mains rate, 5.75 Hz, range 4.5 .. 6.5 Hz.
    /// Derived rather than asserted: `kScanHz · 50/60`, the mains-ratio scaling
    /// of the same motor gearing, because the European rate was not
    /// independently verified either and inventing a second number would be
    /// worse than scaling the first.
    static constexpr double kScanHz50 = kScanHz * 50.0 / 60.0;

    /// [design parameter] total line delay 1.0 ms, range 0.6 .. 1.4 ms.
    /// Documented line-box delay is about a millisecond.
    static constexpr double kLineDelayMs = 1.0;

    /// [design parameter] 18 taps, range 12 .. 24.
    ///
    /// This is a build-time calibration constant and deliberately NOT a live
    /// parameter, because it does not appear in the shipped arithmetic at all —
    /// see `process` for why. It records how many taps the physical line box is
    /// being taken to have; the range is bounded by "enough that the tap
    /// crossfade is indistinguishable from a continuous sweep".
    static constexpr int kScannerTaps = 18;

    /// [design parameter] the three vibrato depths, as fractions of the line.
    /// The ORDERING is documented behaviour; the values are engineering
    /// defaults.
    static constexpr double kV1 = 0.33;
    static constexpr double kV2 = 0.66;
    static constexpr double kV3 = 1.00;

    /// [design parameter] chorus dry blend 0.5, range 0 .. 1. That chorus is
    /// "vibrato plus dry" is documented; the equal blend is a default.
    static constexpr double kChorusMix = 0.5;

    /// Parameter-range maxima used for buffer sizing.
    static constexpr double kMaxLineDelayMs = 1.4;

    /// The measured constructive-sum bound, in linear gain. Feedforward, so
    /// this is dry + wet peaking together, not a loop gain. The suite's sweep
    /// measures 1.16 across the whole mode and parameter space; this is that
    /// maximum rounded up, with the headroom covering grid coarseness rather
    /// than covering a guess.
    /// [design parameter] default 1.5 (+3.5 dB), range 1.2 .. 4.0.
    static constexpr double kWorstCaseGain = 1.5;

    ScannerVibratoT() { update(); }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        line_.prepare(worst_case_delay_samples(sample_rate_));
        scanner_.prepare(sample_rate_);
        scanner_.set_wave(LfoWave::triangle);
        blocker_.set_pole(static_cast<SampleType>(
            1.0 - kDcCornerHz * leslie_detail::kTwoPi / sample_rate_));
        update();
        reset();
    }

    void reset() {
        line_.reset();
        scanner_.reset();
        blocker_.reset();
    }

    void set_mode(Mode mode) {
        mode_ = mode;
        update();
    }

    Mode mode() const { return mode_; }

    void set_scan_hz(double hz) {
        scan_hz_ = std::clamp(hz, 0.0, 100.0);
        update();
    }

    void set_line_ms(double ms) {
        line_ms_ = std::clamp(ms, 0.0, kMaxLineDelayMs);
        update();
    }

    void set_v1_frac(double frac) {
        v1_ = std::clamp(frac, 0.0, 1.0);
        update();
    }
    void set_v2_frac(double frac) {
        v2_ = std::clamp(frac, 0.0, 1.0);
        update();
    }
    void set_v3_frac(double frac) {
        v3_ = std::clamp(frac, 0.0, 1.0);
        update();
    }

    void set_chorus_mix(double mix01) {
        chorus_mix_ = std::clamp(mix01, 0.0, 1.0);
        update();
    }

    // ── Observables ───────────────────────────────────────────────────────

    /// The fraction of the line the scanner currently traverses.
    double depth_fraction() const { return depth_frac_; }

    /// How much dry is blended back. Zero in every V mode — that is what makes
    /// V and C different effects rather than two depths of one effect.
    double dry_mix() const { return dry_mix_; }

    /// The peak fractional pitch shift this setting produces, `|Δf/f|`.
    ///
    /// A linearly ramping delay is a CONSTANT pitch shift, so the depth follows
    /// from the slope alone: the scanner traverses `depth·line` seconds of
    /// delay in each half period, giving `Δf/f = depth·line / (1/(2·rate))`.
    /// Published here so the acceptance test can predict the shift from the
    /// shipped constants instead of restating a number.
    double peak_pitch_shift_ratio() const {
        return depth_frac_ * line_ms_ * 0.001 * 2.0 * scan_hz_;
    }

    /// Zero, for the same reason as the Leslie: the line box's delay is the
    /// modelled electrical path, not processing latency.
    static constexpr int latency_samples() { return 0; }

    SampleType process(SampleType input) {
        if (mode_ == Mode::off) return input;

        const double dry = static_cast<double>(input);
        const double x = static_cast<double>(blocker_.process(input));

        // The scanner position, `[0, 1]`, from the shared LFO in triangle mode.
        // The triangle is the point, not a stand-in for a sine: a constant-slope
        // ramp is a constant pitch shift with a sign flip at each turnaround,
        // which is the boxier motion that keeps this from sounding like every
        // sine-LFO chorus. Do not smooth it.
        const double s = static_cast<double>(scanner_.next_unipolar());

        // Physically the pickup sits between two taps and the scanner
        // crossfades them:
        //
        //     tap_k delay = (k / (N−1)) · line_ms
        //     pos = depth · s · (N−1);  k = floor(pos);  f = pos − k
        //     wet = (1−f)·read(tap_k) + f·read(tap_{k+1})
        //
        // For an equally spaced line that linear crossfade between two taps IS
        // a linear interpolation of one continuous delay, so reading the single
        // fractional delay `depth·s·line_ms` directly is the same signal for
        // any tap count — and reading it with the cubic kernel is strictly
        // better than the two-tap linear blend. The tap count survives as
        // `kScannerTaps`, documenting what is being modelled, rather than as
        // arithmetic that would only coarsen the result.
        const double delay_samples = depth_frac_ * s * line_ms_ * 0.001 * sample_rate_;

        line_.push(static_cast<SampleType>(x));
        const int index = static_cast<int>(std::floor(delay_samples));
        const double frac = delay_samples - static_cast<double>(index);
        const double wet = Interpolator::lagrange(frac,
                                                  static_cast<double>(line_.read(index - 1)),
                                                  static_cast<double>(line_.read(index)),
                                                  static_cast<double>(line_.read(index + 1)),
                                                  static_cast<double>(line_.read(index + 2)));

        // V modes are pure pitch vibrato: the pitch moves, the level does not.
        // C modes add the dry back, so a stationary-pitch copy beats against a
        // moving-pitch one and the result is a sweeping comb — shimmer and
        // thickness rather than wobble. They are DIFFERENT EFFECTS, not two
        // points on a depth control.
        return static_cast<SampleType>(snap_to_zero(dry_mix_ * dry + (1.0 - dry_mix_) * wet));
    }

    void process_block(const SampleType* in, SampleType* out, int n) {
        for (int i = 0; i < n; ++i) out[i] = process(in[i]);
    }

    static int worst_case_delay_samples(double sample_rate) {
        return static_cast<int>(std::ceil(kMaxLineDelayMs * 0.001 * sample_rate)) +
               leslie_detail::kInterpolatorMargin;
    }

private:
    /// [design parameter] default 5 Hz, range 1 .. 20 Hz. Only has to clear
    /// offset ahead of the line; well below anything musical.
    static constexpr double kDcCornerHz = 5.0;

    void update() {
        switch (mode_) {
            case Mode::off:
                depth_frac_ = 0.0;
                dry_mix_ = 1.0;
                break;
            case Mode::v1:
                depth_frac_ = v1_;
                dry_mix_ = 0.0;
                break;
            case Mode::v2:
                depth_frac_ = v2_;
                dry_mix_ = 0.0;
                break;
            case Mode::v3:
                depth_frac_ = v3_;
                dry_mix_ = 0.0;
                break;
            case Mode::c1:
                depth_frac_ = v1_;
                dry_mix_ = chorus_mix_;
                break;
            case Mode::c2:
                depth_frac_ = v2_;
                dry_mix_ = chorus_mix_;
                break;
            case Mode::c3:
                depth_frac_ = v3_;
                dry_mix_ = chorus_mix_;
                break;
        }
        scanner_.set_rate_hz(scan_hz_);
    }

    double sample_rate_ = 48000.0;
    Mode mode_ = Mode::off;
    double scan_hz_ = kScanHz;
    double line_ms_ = kLineDelayMs;
    double v1_ = kV1;
    double v2_ = kV2;
    double v3_ = kV3;
    double chorus_mix_ = kChorusMix;
    double depth_frac_ = 0.0;
    double dry_mix_ = 1.0;

    DelayLineT<SampleType> line_{};
    LfoT<double> scanner_{};
    DcBlocker<SampleType> blocker_{};
};

using ScannerVibrato = ScannerVibratoT<float>;
using ScannerVibrato64 = ScannerVibratoT<double>;

}  // namespace pulp::signal
