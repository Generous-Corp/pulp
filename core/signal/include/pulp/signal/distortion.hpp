#pragma once

/// @file distortion.hpp
/// The circuit-modelled clipper family: diode-to-ground and diode-in-the-loop,
/// solved as ODEs with memory.
///
/// This is the distortion family WITH MEMORY, and that boundary is the reason
/// it is a separate header from `saturator.hpp` rather than a mode on it. A
/// capacitor sits inside the clipping network, so the effective clip point at
/// this instant depends on the filtered recent history of the signal, not just
/// on the current sample. That is what makes Newton-Raphson on an ODE the right
/// tool instead of a static `y = f(x)` table — and it is why a "clean tanh"
/// character belongs in `SaturatorT`, not as a memoryless mode bolted on here.
/// The `C = 0` degenerate configuration exists to test the solver's limit case,
/// not as a shipped preset.
///
/// Three classes, one header, because they share the ODE solver. Splitting them
/// per class would mean either duplicating that solver or inventing a fourth
/// header to hold it; the solver is the coupling, so the coupling stays in one
/// file.
///
/// ## The one nonlinearity
///
/// Everything here is the Shockley diode law, `i_D(v) = Is·(exp(v/(n·Vt)) − 1)`
/// (Shockley 1949, as used throughout the circuit-modelling literature — Yeh,
/// Abel & Smith 2010, §II). Silicon, germanium and LED differ ONLY in `(Is, n)`.
/// The audible consequences — germanium turns on ~150–200 mV earlier and
/// softer, LEDs latest and hardest — fall straight out of plugging each row
/// into the one equation. That is series law 7 applied to diode families: find
/// the dimensionless shape rather than interpolating fitted curves.
///
/// None of the `(Is, n)` pairs are measured parts. Each is an honest
/// `[design parameter]` representing the typical published order of magnitude
/// for a device CLASS.
///
/// ## Symmetry as a topology, not a formula
///
/// The antiparallel network is parameterised by how many diodes are in series
/// on each leg, because that is the physical thing that changes:
///
/// ```
/// i(v) = Is·( exp(a·v/θ) − exp(−b·v/θ) )        θ = n·Vt
/// ```
///
/// where `a` and `b` are the RECIPROCAL series counts of the two legs. `a = b =
/// 1` is the matched pair (odd, symmetric, odd harmonics only). `a = 1/2` is two
/// diodes in series on one leg, turning on at twice the threshold — mild
/// asymmetry. `b = 0` removes the second leg entirely: a single diode, half-wave
/// clipping, strong even harmonics.
///
/// Reciprocal counts rather than counts is what makes the parameter continuous:
/// "leg absent" is `b = 0`, a finite value, where a count would have to be
/// infinite. `symmetry ∈ [−1, +1]` maps onto them directly, and the crossfade
/// interpolates the dimensionless shape rather than blending two independently
/// fitted waveforms.
///
/// ## Why trapezoidal rule plus Newton
///
/// The clipper ODE is stiff near the diode knee — `Vt` is 26 mV against a
/// signal swinging volts — so explicit methods need prohibitive sample rates to
/// stay stable. That is precisely Yeh, Abel & Smith's finding for this class of
/// ODE. Trapezoidal rule is implicit, second-order and A-stable, and its inner
/// solve is one Newton iteration loop with a HARD ITERATION CAP: an unbounded
/// loop in `process()` is not RT-safe, and hitting the cap clamps to the last
/// iterate rather than crashing.
///
/// ## Patch notes
///
/// - **`to_ground`, symmetric, high drive** — the Distortion archetype: hard,
///   bright, level-independent knee. Pair with a low post-tone corner to tame
///   fizz.
/// - **`to_ground`, `symmetry = −1`** — half-wave, fuzz-adjacent: strong even
///   harmonics read as buzzier and more lo-fi than the matched pair.
/// - **`in_loop`, moderate drive** — the Overdrive archetype. The knee TRACKS
///   the drive rather than sitting at a fixed voltage, so picking harder moves
///   the knee itself, not just the level. That is the "boost that also
///   compresses" feel.
/// - **Germanium** — earlier, softer onset. **LED** — latest onset, steepest
///   slope once clipping starts.
///
/// RT contract: `prepare()` may allocate (it configures the oversampler when
/// one is in use). `set_*`, `process()` and `reset()` never allocate, never
/// lock, never perform I/O. `process()` costs at most `kMaxNewtonIterations`
/// iterations of one `exp` pair — bounded, never data-dependent past that cap.
/// There is no randomness anywhere in this file, so determinism (series law 2)
/// is satisfied by construction; that is worth stating because it is easy to
/// assume a "vintage" effect needs drift, and this one does not.
///
/// References: Shockley (1949) for the diode law. Yeh, Abel & Smith,
/// "Simplified, Physically-Informed Models of Distortion and Overdrive Guitar
/// Effects Pedals" (DAFx-07 / IEEE TASLP 2010) for the ODE formulation and the
/// numerical-methods survey that selects TR. ElectroSmash's public Tube
/// Screamer circuit analysis for the in-loop topology and its documented
/// behaviour ONLY — no component values are reproduced, and every default here
/// is independently chosen and independently ranged.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/junction.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace pulp::signal {

/// Diode device families. The members differ only in `(Is, n)`.
enum class DiodeModel : std::uint8_t {
    silicon,    ///< Small-signal switching class. Clips ~0.6 V, hard.
    germanium,  ///< Much larger `Is`: turns on earlier and softer.
    led,        ///< Smallest `Is`, highest `n`: latest onset, steepest slope.
};

/// Where the diode pair sits relative to the gain stage.
enum class ClipperTopology : std::uint8_t {
    to_ground,  ///< Shunt across the stage output. Level-independent knee.
    in_loop,    ///< Across the op-amp's feedback resistor. Knee tracks drive.
};

namespace detail {

/// The `(Is, n)` pair for a device family. Honest `[design parameter]` rows
/// representing a class's published order of magnitude, never a measured part.
///
/// [design parameter] silicon Is 1e-14 (range 1e-15 .. 1e-12), n 1.0 (1.0 .. 1.05)
/// [design parameter] germanium Is 1e-6 (range 1e-7 .. 1e-5), n 1.1 (1.0 .. 1.3)
/// [design parameter] led Is 1e-18 (range 1e-19 .. 1e-17), n 2.0 (1.5 .. 2.5)
inline void apply_diode_model(junction::JunctionPair& network, DiodeModel model) {
    switch (model) {
        case DiodeModel::germanium:
            network.saturation_current = 1e-6;
            network.ideality = 1.1;
            break;
        case DiodeModel::led:
            network.saturation_current = 1e-18;
            network.ideality = 2.0;
            break;
        case DiodeModel::silicon:
        default:
            network.saturation_current = 1e-14;
            network.ideality = 1.0;
            break;
    }
}

/// `symmetry ∈ [−1, +1]` → the two legs' reciprocal series counts.
///
/// Positive `symmetry` lengthens the positive leg (turns on later); negative
/// `symmetry` fades the negative leg out entirely. Both are continuous and both
/// reduce to the matched pair at 0.
inline void apply_symmetry(junction::JunctionPair& network, double symmetry) {
    const double s = std::clamp(symmetry, -1.0, 1.0);
    network.leg_a = 1.0 / (1.0 + std::max(0.0, s));
    network.leg_b = 1.0 + std::min(0.0, s);
}

}  // namespace detail

/// Shared solver constants. Public because both clipper classes and their tests
/// cite them, and because the iteration cap is an RT-safety contract rather
/// than an implementation detail.
struct ClipperSolverConfig;

namespace detail {

/// One trapezoidal-rule step of `C·dv/dt = forcing + (vin − v)/R − i_D(v)`,
/// solved by a SAFEGUARDED Newton iteration.
///
/// Plain Newton from the previous sample — what the specification calls for —
/// diverges here, and it is worth being precise about why rather than just
/// damping it. The residual's curvature is set by `exp(v/θ)` with `θ ≈ 26 mV`,
/// so a single unguarded step taken from outside the conducting region
/// overshoots by orders of magnitude and lands where the exponential is
/// astronomically large; the next step is worse. In practice the iterate
/// escapes to ±10¹⁶ within a few samples of a cold start and never returns.
///
/// The fix is the standard one for monotone root finds: keep a bracket and fall
/// back to bisection whenever a Newton step would leave it. This is globally
/// convergent — `F` is strictly increasing in `v`, since every term of `F'` is
/// positive — while still getting Newton's quadratic convergence once the
/// iterate is close, which is the common case at audio rates where consecutive
/// samples differ little.
///
/// The bracket is physical rather than arbitrary: the diode network can only
/// sink current toward ground, so the node voltage always lies between where it
/// was and where the linear network alone would drive it.
///
/// Returns the solved voltage and writes the iteration count taken, so the
/// RT-safety cap can be asserted rather than assumed.
template <typename Residual, typename Derivative>
inline double solve_safeguarded_newton(Residual&& f, Derivative&& df, double lo, double hi,
                                       double initial, double alternate, double tolerance,
                                       int max_iterations, int& iterations_out) {
    // Order the bracket and pad it, so a root sitting exactly on an endpoint is
    // still strictly interior.
    if (lo > hi) std::swap(lo, hi);
    const double pad = 1e-9 + 0.01 * (hi - lo);
    lo -= pad;
    hi += pad;

    double f_lo = f(lo);
    double f_hi = f(hi);
    // If the physical bracket does not actually straddle the root, widen a
    // bounded number of times rather than failing. Bounded because an unbounded
    // widening loop is exactly the RT hazard the iteration cap exists to
    // prevent.
    for (int widen = 0; widen < 8 && f_lo * f_hi > 0.0; ++widen) {
        const double span = std::max(hi - lo, 1e-3);
        lo -= span;
        hi += span;
        f_lo = f(lo);
        f_hi = f(hi);
    }

    // Two candidate starts: where the solve landed last sample (excellent when
    // the signal is moving slowly, which is the common case) and the analytic
    // conduction estimate (excellent on a cold start or a fast transient).
    // Taking the better of the two by residual costs two evaluations and saves
    // the dozens of iterations a bad start would need.
    double v = std::clamp(initial, lo, hi);
    const double candidate = std::clamp(alternate, lo, hi);
    if (std::abs(f(candidate)) < std::abs(f(v))) v = candidate;
    int iterations = 0;
    for (; iterations < max_iterations; ++iterations) {
        const double fv = f(v);
        if (std::abs(fv) < tolerance) break;

        // Maintain the bracket from every evaluation, so the fallback keeps
        // getting tighter even while Newton is doing the work.
        if (f_lo * fv < 0.0) {
            hi = v;
            f_hi = fv;
        } else {
            lo = v;
            f_lo = fv;
        }

        const double dfv = df(v);
        double next = dfv > 0.0 ? v - fv / dfv : v;
        // Outside the bracket, or non-finite: bisect instead. This is what
        // makes the iteration globally convergent rather than merely usually
        // convergent.
        if (!std::isfinite(next) || next <= lo || next >= hi) next = 0.5 * (lo + hi);
        v = next;
    }
    iterations_out = iterations;
    return std::isfinite(v) ? v : 0.0;
}

}  // namespace detail

struct ClipperSolverConfig {
    /// Convergence threshold on `|F(v)|`. Tighter costs iterations; looser
    /// risks visible jitter at the knee.
    /// [design parameter] default 1e-9, range 1e-11 .. 1e-6.
    static constexpr double kNewtonTolerance = 1e-9;

    /// Hard cap on inner-loop iterations per sample. An unbounded loop in
    /// `process()` is not RT-safe; hitting the cap clamps to the last iterate,
    /// which is a bounded worst-case degrade rather than a failure.
    /// [design parameter] default 8, range 4 .. 16.
    static constexpr int kMaxNewtonIterations = 8;

    /// ADAA fallback guard on `|v[n] − v[n−1]|`. Below this the difference
    /// quotient loses precision and the plain evaluation is used.
    /// [design parameter] default 1e-5, range 1e-7 .. 1e-3.
    static constexpr double kAdaaEpsilon = 1e-5;
};

/// The diode-to-ground clipper: series resistor, shunt diode network, optional
/// shunt capacitor.
///
/// `C·dv/dt = (vin − v)/R − i_D(v)`, solved by trapezoidal rule with a
/// Newton-Raphson inner solve.
template <typename SampleType = float>
class DiodeClipperT {
public:
    using Model = DiodeModel;

    /// Series input resistor. Sets the ODE's time-constant scale jointly with
    /// `C`. Not a reproduction of any pedal's input resistor.
    /// [design parameter] default 10 kΩ, range 1 kΩ .. 100 kΩ.
    static constexpr double kResistanceDefault = 10000.0;

    /// Shunt capacitor — the memory element. Rolls the generated harmonics off
    /// before they alias and softens the clip corners.
    /// [design parameter] default 51 pF, range 0 .. 500 pF.
    static constexpr double kCapacitanceDefault = 51e-12;

    DiodeClipperT() {
        detail::apply_diode_model(network_, Model::silicon);
        detail::apply_symmetry(network_, 0.0);
    }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        timestep_ = 1.0 / sample_rate_;
        reset();
    }

    void set_diode_model(Model model) { detail::apply_diode_model(network_, model); }

    /// −1 = single diode (half-wave), 0 = matched pair, +1 = mismatched pair.
    void set_symmetry(double symmetry) { detail::apply_symmetry(network_, symmetry); }

    void set_resistance(double ohms) { resistance_ = std::max(ohms, 1.0); }

    /// Zero is the degenerate memoryless configuration: a solver limit case for
    /// tests, not a shipped preset.
    void set_capacitance(double farads) { capacitance_ = std::max(farads, 0.0); }

    /// Antiderivative antialiasing is NOT offered on this class, deliberately.
    ///
    /// It was, and it diverged: a 440 Hz full-scale sine reached 5.8e25, growing
    /// geometrically at roughly 3300x per sample, on every diode model and at
    /// 8/48/192 kHz. It never reported failure because every value stayed
    /// finite, and no test saw it because the only ADAA assertion compared two
    /// renders for equality — which a deterministic divergence passes.
    ///
    /// The cause is structural, not a bug to patch. ADAA replaces the diode term
    /// with its MEAN over `[v(n-1), v(n)]`, but trapezoidal rule is already
    /// averaging the nonlinearity across the timestep. Stacking the two
    /// double-counts the averaging, so the residual's root moves outside the
    /// bracket `[min(0, v(n-1), vin), max(...)]` that makes the safeguarded
    /// solve safe; the solver then clamps to an endpoint and the state ratchets
    /// away. Matching the Jacobian to the residual (which was ALSO wrong here —
    /// it used the plain conductance) is necessary and not sufficient: with a
    /// finite-difference-verified derivative it still diverges.
    ///
    /// Doing this properly means antialiasing the explicit output shaping rather
    /// than the implicit term, which is a different design and wants its own
    /// validation. Until then the honest position is that the x1 path has no
    /// antialiasing and the oversampled tiers are how you get it.
    ///
    /// `JunctionPair::adaa_current` remains, and remains correct: it is sound
    /// for a MEMORYLESS shaper, which is how `diode_bridge_compressor.hpp` uses
    /// it. The unsoundness is specific to putting it inside this implicit solve.

    void reset() {
        voltage_ = 0.0;
        previous_input_ = 0.0;
        previous_voltage_ = 0.0;
        last_iterations_ = 0;
    }

    /// Iterations the last solve took. Exposed so the RT-safety cap can be
    /// asserted rather than assumed.
    int last_iteration_count() const { return last_iterations_; }

    SampleType process(SampleType input) {
        const double vin = static_cast<double>(input);
        const double v = solve(vin);
        previous_input_ = vin;
        previous_voltage_ = v;
        voltage_ = v;
        return static_cast<SampleType>(snap_to_zero(v));
    }

    /// The node voltage without advancing — for tests that check the ODE's
    /// residual directly.
    double voltage() const { return voltage_; }

    /// `(vin − v)/R − i_D(v)`, the residual the memoryless (`C = 0`) solution
    /// must drive to zero.
    double resistive_residual(double vin, double v) const {
        return (vin - v) / resistance_ - network_.current(v);
    }

private:
    /// The diode term as the solver sees it: the plain current, or its ADAA
    /// difference quotient. The guard and the fallback live in `JunctionPair`,
    /// so the two clipper classes cannot drift apart on either.
    double diode_term(double v) const {
        return network_.current(v);
    }

    /// `d/dv` of `diode_term`. It MUST follow whichever branch `diode_term`
    /// took: a Newton step built from the plain `conductance()` while the
    /// residual uses the ADAA quotient is solving one equation with another
    /// equation's Jacobian, and that diverges rather than converging slowly.
    /// See `JunctionPair::adaa_conductance` for the measured consequence.
    double diode_slope(double v) const {
        return network_.conductance(v);
    }

    double solve(double vin) {
        // Trapezoidal rule on C·dv/dt = (vin − v)/R − i_D(v). With C = 0 the
        // derivative term drops out and this becomes the memoryless
        // transcendental equation, solved by the same routine.
        const double c_over_t = capacitance_ / timestep_;
        const double history =
            0.5 * ((previous_input_ - previous_voltage_) / resistance_ -
                   network_.current(previous_voltage_));

        // With no capacitor the ODE is not an ODE: `C·dv/dt` vanishes and the
        // circuit equation is purely algebraic, `(vin − v)/R = i_D(v)`. Pushing
        // that through trapezoidal rule would NOT solve it — TR at `C = 0`
        // enforces `g(vₙ) = −g(vₙ₋₁)`, so the residual alternates sign every
        // sample instead of vanishing. That is the textbook non-L-stable
        // behaviour of TR at a stiff limit, not a bug in the solve, and it is
        // why the degenerate case gets the algebraic equation directly.
        const bool memoryless = capacitance_ <= 0.0;
        const auto residual = [&](double v) {
            if (memoryless) return diode_term(v) - (vin - v) / resistance_;
            return c_over_t * (v - previous_voltage_) -
                   0.5 * ((vin - v) / resistance_ - diode_term(v)) - history;
        };
        // Every term is strictly positive, so the residual is strictly
        // increasing in v — which is what makes the bracketed fallback safe.
        const auto derivative = [&](double v) {
            if (memoryless) return diode_slope(v) + 1.0 / resistance_;
            return c_over_t + 0.5 / resistance_ + 0.5 * diode_slope(v);
        };

        return detail::solve_safeguarded_newton(
            residual, derivative,
            /*lo=*/std::min({0.0, previous_voltage_, vin}),
            /*hi=*/std::max({0.0, previous_voltage_, vin}),
            /*initial=*/previous_voltage_,
            /*alternate=*/network_.conduction_estimate(vin, resistance_),
            ClipperSolverConfig::kNewtonTolerance,
            ClipperSolverConfig::kMaxNewtonIterations, last_iterations_);
    }

    junction::JunctionPair network_{};
    double sample_rate_ = 44100.0;
    double timestep_ = 1.0 / 44100.0;
    double resistance_ = kResistanceDefault;
    double capacitance_ = kCapacitanceDefault;

    double voltage_ = 0.0;
    double previous_input_ = 0.0;
    double previous_voltage_ = 0.0;
    int last_iterations_ = 0;
};

using DiodeClipper = DiodeClipperT<float>;
using DiodeClipper64 = DiodeClipperT<double>;

/// The op-amp gain stage with the diode network either at its output or across
/// its feedback resistor.
///
/// The in-loop KCL at the virtual ground reduces to the SAME functional form as
/// the to-ground ODE with `v ≡ −vout`, `R ≡ Rf`, `C ≡ Cf`, plus a `vin/Rin`
/// forcing term. So this reuses the identical trapezoidal + Newton solver
/// rather than introducing a second numerical method.
///
/// **Small-signal gain and its bound (series law 1).** At `v → 0` the diodes'
/// incremental conductance is `Is/(n·Vt)` — for silicon about 3.9e-13 S, utterly
/// negligible against `1/Rf` — so at zero signal the diodes are effectively an
/// open circuit and the small-signal gain is exactly the linear `Rf/Rin`. As
/// `|v|` grows the diodes' conductance grows exponentially, shunting more
/// feedback current and REDUCING loop gain monotonically. The loop is therefore
/// self-limiting, never self-amplifying, and `Rf/Rin` bounds it above at every
/// amplitude. That is the tested invariant the Forge registry cites.
template <typename SampleType = float>
class FeedbackClipperT {
public:
    using Model = DiodeModel;
    using Topology = ClipperTopology;

    /// Feedback resistor: sets the linear gain `Rf/Rin` and, with `Cf`, the
    /// in-loop knee corner.
    /// [design parameter] default 51 kΩ, range 10 kΩ .. 200 kΩ.
    static constexpr double kFeedbackResistanceDefault = 51000.0;

    /// Input resistor. `Rf/Rin` is the stage's linear gain.
    /// [design parameter] default 4.7 kΩ, range 1 kΩ .. 100 kΩ.
    static constexpr double kInputResistanceDefault = 4700.0;

    /// In-loop knee corner, `fc = 1/(2π·Rf·Cf)`. Above it the capacitor shunts
    /// feedback current around the diodes, so high harmonics see less
    /// clipping-loop gain — the mechanism behind the smoother, brighter-forgiving
    /// character of this topology. The default sits in the documented
    /// behavioural neighbourhood, not on any schematic.
    /// [design parameter] default 720 Hz, range 100 Hz .. 5000 Hz.
    static constexpr double kKneeCornerHzDefault = 720.0;

    FeedbackClipperT() {
        detail::apply_diode_model(network_, Model::silicon);
        detail::apply_symmetry(network_, 0.0);
        update_capacitance();
    }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        timestep_ = 1.0 / sample_rate_;
        ground_stage_.prepare(sample_rate_);
        reset();
    }

    void set_topology(Topology topology) { topology_ = topology; }
    Topology topology() const { return topology_; }

    void set_diode_model(Model model) {
        detail::apply_diode_model(network_, model);
        ground_stage_.set_diode_model(model);
    }

    void set_symmetry(double symmetry) {
        detail::apply_symmetry(network_, symmetry);
        ground_stage_.set_symmetry(symmetry);
    }

    void set_feedback_resistance(double ohms) {
        feedback_resistance_ = std::max(ohms, 1.0);
        update_capacitance();
    }

    void set_input_resistance(double ohms) { input_resistance_ = std::max(ohms, 1.0); }

    void set_knee_corner_hz(double hz) {
        knee_corner_hz_ = std::clamp(hz, 1.0, 20000.0);
        update_capacitance();
    }


    /// The stage's linear (small-signal) gain, `Rf/Rin` — the bound the loop can
    /// never exceed at frequencies below the feedback capacitor's corner. Above
    /// that corner the capacitor shunts feedback current and the gain is lower,
    /// which is the topology's defining behaviour rather than a caveat.
    double linear_gain() const { return feedback_resistance_ / input_resistance_; }

    void reset() {
        previous_input_ = 0.0;
        previous_voltage_ = 0.0;
        last_iterations_ = 0;
        ground_stage_.reset();
    }

    int last_iteration_count() const {
        return topology_ == Topology::to_ground ? ground_stage_.last_iteration_count()
                                                : last_iterations_;
    }

    SampleType process(SampleType input) {
        const double vin = static_cast<double>(input);
        if (topology_ == Topology::to_ground) {
            // A plain linear gain stage with the network shunting its output —
            // which IS the diode clipper fed by a pre-gain, so it composes one
            // rather than re-deriving the same circuit with the wrong
            // capacitor. That distinction is not cosmetic: the feedback cap
            // that sets the in-loop knee sits at 720 Hz, and reusing it here
            // would put a shunt clipper behind an audio-band filter, which
            // measurably tilts the supposedly level-independent plateau this
            // topology exists to provide.
            return ground_stage_.process(static_cast<SampleType>(vin * linear_gain()));
        }

        // In-loop: the same ODE with v ≡ −vout, R ≡ Rf, C ≡ Cf, and the input
        // current vin/Rin as an extra forcing term.
        const double forcing = vin / input_resistance_;
        const double v = solve(0.0, forcing, feedback_resistance_);
        previous_input_ = 0.0;
        previous_voltage_ = v;
        // v is −vout by the substitution, so the stage's output is its negation
        // — an inverting stage, which is what the topology is.
        return static_cast<SampleType>(snap_to_zero(-v));
    }

private:
    void update_capacitance() {
        constexpr double two_pi = 6.283185307179586476925286766559;
        capacitance_ = 1.0 / (two_pi * feedback_resistance_ * knee_corner_hz_);
    }

    /// The diode term as the solver sees it: the plain current, or its ADAA
    /// difference quotient. The guard and the fallback live in `JunctionPair`,
    /// so the two clipper classes cannot drift apart on either.
    double diode_term(double v) const {
        return network_.current(v);
    }

    /// `d/dv` of `diode_term`. It MUST follow whichever branch `diode_term`
    /// took: a Newton step built from the plain `conductance()` while the
    /// residual uses the ADAA quotient is solving one equation with another
    /// equation's Jacobian, and that diverges rather than converging slowly.
    /// See `JunctionPair::adaa_conductance` for the measured consequence.
    double diode_slope(double v) const {
        return network_.conductance(v);
    }

    /// The shared trapezoidal + Newton solve. `forcing` is the extra current
    /// injected at the node (zero for the to-ground placement).
    double solve(double vin, double forcing, double resistance) {
        const double c_over_t = capacitance_ / timestep_;
        const double history =
            0.5 * (forcing + (previous_input_ - previous_voltage_) / resistance -
                   network_.current(previous_voltage_));

        const auto residual = [&](double v) {
            return c_over_t * (v - previous_voltage_) -
                   0.5 * (forcing + (vin - v) / resistance - diode_term(v)) - history;
        };
        const auto derivative = [&](double v) {
            return c_over_t + 0.5 / resistance + 0.5 * diode_slope(v);
        };

        // Where the linear network alone would drive the node: the forcing
        // current through the feedback resistor, plus any direct drive.
        const double open_circuit = vin + forcing * resistance;
        return detail::solve_safeguarded_newton(
            residual, derivative,
            /*lo=*/std::min({0.0, previous_voltage_, open_circuit}),
            /*hi=*/std::max({0.0, previous_voltage_, open_circuit}),
            /*initial=*/previous_voltage_,
            /*alternate=*/network_.conduction_estimate(open_circuit, resistance),
            ClipperSolverConfig::kNewtonTolerance,
            ClipperSolverConfig::kMaxNewtonIterations, last_iterations_);
    }

    junction::JunctionPair network_{};
    Topology topology_ = Topology::in_loop;
    double sample_rate_ = 44100.0;
    double timestep_ = 1.0 / 44100.0;
    double feedback_resistance_ = kFeedbackResistanceDefault;
    double input_resistance_ = kInputResistanceDefault;
    double knee_corner_hz_ = kKneeCornerHzDefault;
    double capacitance_ = 0.0;

    double previous_input_ = 0.0;
    double previous_voltage_ = 0.0;
    int last_iterations_ = 0;

    /// The to-ground realization, composed rather than re-derived.
    DiodeClipperT<SampleType> ground_stage_{};
};

using FeedbackClipper = FeedbackClipperT<float>;
using FeedbackClipper64 = FeedbackClipperT<double>;

/// Pre/post tone shaping around the clipper: a two-pole tilt pair.
///
/// Deliberately NOT a full three-band passive stack — a literal Baxandall/TMB
/// network has its own interacting-pole topology and is its own module. What
/// this is for:
///
/// - **pre** shapes what the clipper SEES. Rolling lows off before the clip
///   emphasises high-harmonic buzz; rolling highs off keeps the fundamental
///   dominant through the nonlinearity. Standard pre-emphasis practice.
/// - **post** is the de-fizz stage every distortion needs after harmonic
///   generation. For the to-ground topology this is the shunt capacitor's
///   corner made externally controllable; for in-loop the equivalent corner is
///   buried inside the feedback network, so exposing it here gives both
///   topologies the same control.
///
/// Both stages are the house TPT one-pole; the filter math is not re-derived.
///
/// RT contract: as the clippers. No allocation, no locks.
template <typename SampleType = float>
class ToneStackT {
public:
    /// Pre-emphasis shelf gain. Default flat, so the corner alone does the
    /// shaping rather than an added gain stage.
    /// [design parameter] default 0 dB, range ±12 dB.
    static constexpr double kPreGainDbMax = 12.0;

    void prepare(double sample_rate) {
        pre_.prepare(static_cast<SampleType>(sample_rate));
        post_.prepare(static_cast<SampleType>(sample_rate));
        reset();
    }

    void set_pre_tone_hz(double hz) {
        pre_hz_ = std::clamp(hz, 20.0, 20000.0);
        pre_.set_cutoff(static_cast<SampleType>(pre_hz_));
    }

    void set_post_tone_hz(double hz) {
        post_hz_ = std::clamp(hz, 20.0, 20000.0);
        post_.set_cutoff(static_cast<SampleType>(post_hz_));
    }

    /// Pre-emphasis shelf gain in dB. Positive lifts content above the corner
    /// into the clipper, negative keeps the fundamental dominant.
    void set_pre_gain_db(double db) {
        pre_gain_ = units::db_to_linear(std::clamp(db, -kPreGainDbMax, kPreGainDbMax));
    }

    /// Linear crossfade of the shaped stage against its bypass.
    void set_tone_mix(double mix01) { mix_ = std::clamp(mix01, 0.0, 1.0); }

    void reset() {
        pre_.reset();
        post_.reset();
    }

    /// The pre-emphasis stage: a high shelf built from the one-pole's
    /// complementary outputs, so a gain of 1 is exactly flat by construction
    /// rather than to within a fitted tolerance.
    SampleType process_pre(SampleType input) {
        const auto out = pre_.process(input);
        const double shaped =
            static_cast<double>(out.lowpass) + pre_gain_ * static_cast<double>(out.highpass);
        return static_cast<SampleType>(static_cast<double>(input) +
                                       mix_ * (shaped - static_cast<double>(input)));
    }

    /// The post stage: a plain one-pole low-pass, the de-fizz.
    SampleType process_post(SampleType input) {
        const double filtered = static_cast<double>(post_.process_lowpass(input));
        return static_cast<SampleType>(static_cast<double>(input) +
                                       mix_ * (filtered - static_cast<double>(input)));
    }

private:
    TptFilterT<SampleType> pre_{};
    TptFilterT<SampleType> post_{};
    double pre_hz_ = 720.0;
    double post_hz_ = 4000.0;
    double pre_gain_ = 1.0;
    double mix_ = 1.0;
};

using ToneStack = ToneStackT<float>;
using ToneStack64 = ToneStackT<double>;

}  // namespace pulp::signal
