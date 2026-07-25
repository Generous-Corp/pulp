#pragma once

// Wallace playback loss — the physical tape tier's loss stage.
//
// The model (Wallace 1951; physical ranges from Bertram 1994) is three
// multiplied magnitude terms:
//
//   spacing    H_sp (f) = exp(-k·d)                     k = 2πf / v
//   thickness  H_th (f) = (1 - exp(-k·δ)) / (k·δ)
//   gap        H_gap(f) = sin(k·g/2) / (k·g/2)
//
// They are realized by DIFFERENT filters, because they are different KINDS of
// response and a single realization cannot serve both:
//
//   * spacing × thickness is a smooth monotone tilt whose corner moves with
//     tape speed and head-tape spacing. At the slow, worn end of the age axis
//     — 1.875 ips with 25 µm spacing, which is exactly the cassette-era
//     territory this tier exists for — that corner sits near 100 Hz. A
//     minimum-phase FIR spanning ~2 ms cannot represent a 100 Hz corner at any
//     order the loop can afford: measured 15 dB of error at 97 taps and still
//     2 dB at 513. So the tilt is carried by a LOW-ORDER IIR cascade (two
//     one-poles and a first-order shelf), least-squares fitted to the analytic
//     magnitude. Four parameters buy what several hundred FIR taps could not.
//   * the gap term is a sinc with a genuine null at f = v/g. Nulls are what
//     FIRs are for and no low-order IIR puts one there, so the minimum-phase
//     FIR keeps exactly this term — a purely high-frequency feature it
//     represents comfortably at the specified order.
//
// The gap term depends only on speed and head geometry, never on age, so its
// FIR is designed once per tape speed. Only the IIR moves along the age axis.
//
// NORMATIVE, and a classic trap: when the tape SPEED changes, both filters are
// redesigned and the change is made by running TWO COMPLETE FILTER INSTANCES
// and crossfading their OUTPUTS. Never interpolate IIR coefficients between two
// designs — the state of a recursive filter is meaningful only against the
// coefficients that produced it, so a blended coefficient set is applied to
// state that belongs to neither, which rings or blows up rather than
// crossfading. (Moving the IIR smoothly along the AGE axis is a different
// thing and is fine: those coefficients are recomputed from continuously
// interpolated PHYSICAL parameters — a cutoff in hertz, a shelf in decibels —
// which is ordinary filter modulation, not a blend of two designs.)

#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/windowed_sinc_design.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numeric>
#include <vector>

namespace pulp::signal::chardelay {

/// Head/tape geometry for the Wallace loss model. All lengths in metres.
struct TapeLossGeometry {
    double speed_ips = 7.5;
    double spacing_m = 5e-6;
    double thickness_m = kTapeCoatingThicknessM;
    double gap_m = kTapeHeadGapM;
};

/// Floor applied to a modeled loss magnitude before anything is fitted to it.
/// The physics is unbounded — the spacing term passes −400 dB inside the audio
/// band at the worn end — and fitting to that wastes the model's degrees of
/// freedom on a region that is already inaudible. −60 dB is far enough down to
/// be gone inside a feedback loop that is itself losing energy every pass.
inline constexpr double kTapeLossFloorDb = -60.0;

inline double tape_wavenumber(double frequency_hz, double speed_ips) noexcept {
    const double velocity = std::max(speed_ips * 0.0254, 1e-6);
    return 2.0 * kPi * std::max(frequency_hz, 0.0) / velocity;
}

/// The smooth tilt: spacing × thickness. Carried by the IIR cascade.
inline double tape_loss_smooth_magnitude(double frequency_hz,
                                         const TapeLossGeometry& geometry) noexcept {
    const double wavenumber = tape_wavenumber(frequency_hz, geometry.speed_ips);
    const double spacing_loss = std::exp(-wavenumber * geometry.spacing_m);
    const double thickness_argument = wavenumber * geometry.thickness_m;
    const double thickness_loss = (thickness_argument < 1e-9)
                                      ? 1.0
                                      : (1.0 - std::exp(-thickness_argument)) / thickness_argument;
    return spacing_loss * thickness_loss;
}

/// The gap null. Carried by the minimum-phase FIR.
inline double tape_loss_gap_magnitude(double frequency_hz,
                                      const TapeLossGeometry& geometry) noexcept {
    const double argument =
        tape_wavenumber(frequency_hz, geometry.speed_ips) * geometry.gap_m * 0.5;
    if (argument < 1e-9) return 1.0;
    return std::abs(std::sin(argument) / argument);
}

/// The complete modeled response.
inline double tape_loss_magnitude(double frequency_hz,
                                  const TapeLossGeometry& geometry) noexcept {
    return tape_loss_smooth_magnitude(frequency_hz, geometry) *
           tape_loss_gap_magnitude(frequency_hz, geometry);
}

/// The response the realization actually targets, floor included. The
/// acceptance suite compares the realized cascade against THIS, not against
/// the unbounded physics.
inline double tape_loss_magnitude_floored(double frequency_hz,
                                          const TapeLossGeometry& geometry) noexcept {
    const double floor_linear = std::pow(10.0, kTapeLossFloorDb / 20.0);
    return std::max(tape_loss_magnitude(frequency_hz, geometry), floor_linear);
}

// ── The smooth tilt: two fitted shapes, scaled ────────────────────────────
//
// The key structural fact: neither smooth term depends on frequency, speed and
// spacing independently. The spacing term is exp(-2π·f·d/v) — a function of the
// single product f·d/v — and the thickness term is a function of f·δ/v alone.
// So each is a FIXED dimensionless shape, and every (speed, spacing) the module
// can reach is that same shape with its frequency axis scaled by a scalar.
//
// That is what makes the age axis exact instead of interpolated: a cascade
// fitted once to the dimensionless shape is correct at every age with its
// corners multiplied by v/d. No knots, no parameter interpolation, and a tape
// speed change becomes a multiply rather than a refit.

/// One fitted cascade in dimensionless frequency: corners are in units of the
/// shape's own normalized variable and become hertz when multiplied by that
/// term's scale factor.
template <std::size_t Poles, std::size_t Shelves>
struct LossShape {
    std::array<double, Poles> pole_x{};
    std::array<double, Shelves> shelf_x{};
    std::array<double, Shelves> shelf_db{};
};

using SpacingShape = LossShape<kSpacingPoleSections, kSpacingShelfSections>;
using ThicknessShape = LossShape<kThicknessPoleSections, kThicknessShelfSections>;

/// The realized cascade's parameters in hertz, poles and shelves from both
/// shapes concatenated.
struct TapeLossIirParams {
    std::array<double, kLossPoleSections> pole_hz{};
    std::array<double, kLossShelfSections> shelf_hz{};
    std::array<double, kLossShelfSections> shelf_db{};
};

/// Magnitude of the cascade, in dB. Used by the fitter and by tests.
inline double tape_loss_iir_magnitude_db(const TapeLossIirParams& parameters,
                                         double frequency_hz) noexcept {
    double db = 0.0;
    for (double corner : parameters.pole_hz) {
        const double ratio = frequency_hz / std::max(corner, 1e-9);
        db += -10.0 * std::log10(1.0 + ratio * ratio);
    }
    for (std::size_t i = 0; i < kLossShelfSections; ++i) {
        const double gain = std::pow(10.0, parameters.shelf_db[i] / 20.0);
        const double ratio = frequency_hz / std::max(parameters.shelf_hz[i], 1e-9);
        db += 10.0 * std::log10((1.0 + gain * gain * ratio * ratio) / (1.0 + ratio * ratio));
    }
    return db;
}

/// The two dimensionless targets, in dB, as functions of their own normalized
/// frequency.
inline double spacing_shape_db(double x) noexcept {
    return -8.685889638065035 * 2.0 * kPi * x;  // 20·log10(exp(-2πx))
}

inline double thickness_shape_db(double x) noexcept {
    const double argument = 2.0 * kPi * x;
    if (argument < 1e-9) return 0.0;
    return 20.0 * std::log10((1.0 - std::exp(-argument)) / argument);
}

namespace detail {

using LossSearchVector = std::array<double, kLossSearchDimension>;

/// Generic fit of `Poles` one-poles and `Shelves` first-order shelves to a
/// dimensionless target, minimizing the WORST error in dB over log-spaced
/// points. Minimax rather than least squares because the acceptance criterion
/// is a bound on the worst error at any frequency, and an L2 fit happily trades
/// one large error for many small ones — exactly the wrong bargain. (A tiny L2
/// term breaks ties between parameter sets sharing a worst point.)
template <std::size_t Poles, std::size_t Shelves, typename Target>
inline LossShape<Poles, Shelves> fit_loss_shape(Target&& target) {
    static_assert(Poles + 2u * Shelves <= kLossSearchDimension,
                  "search vector is sized for the largest shape");
    constexpr std::size_t kDimension = Poles + 2u * Shelves;
    const auto points = static_cast<std::size_t>(kLossIirFitPoints);

    std::vector<double> x(points);
    std::vector<double> target_db(points);
    for (std::size_t i = 0; i < points; ++i) {
        x[i] = kLossShapeMinX * std::pow(kLossShapeMaxX / kLossShapeMinX,
                                         static_cast<double>(i) /
                                             static_cast<double>(points - 1));
        target_db[i] = std::max(target(x[i]), kTapeLossFloorDb);
    }

    auto decode = [](const std::array<double, kDimension>& v) {
        LossShape<Poles, Shelves> shape;
        std::size_t k = 0;
        for (std::size_t i = 0; i < Poles; ++i)
            shape.pole_x[i] = std::clamp(std::pow(10.0, v[k++]), 1e-6, 1e4);
        for (std::size_t i = 0; i < Shelves; ++i) {
            shape.shelf_x[i] = std::clamp(std::pow(10.0, v[k++]), 1e-6, 1e4);
            shape.shelf_db[i] = std::clamp(v[k++], kLossShelfMinDb, 0.0);
        }
        return shape;
    };

    auto magnitude = [](const LossShape<Poles, Shelves>& shape, double f) {
        double db = 0.0;
        for (double corner : shape.pole_x) {
            const double r = f / corner;
            db += -10.0 * std::log10(1.0 + r * r);
        }
        for (std::size_t i = 0; i < Shelves; ++i) {
            const double g = std::pow(10.0, shape.shelf_db[i] / 20.0);
            const double r = f / shape.shelf_x[i];
            db += 10.0 * std::log10((1.0 + g * g * r * r) / (1.0 + r * r));
        }
        return db;
    };

    auto objective = [&](const std::array<double, kDimension>& v) {
        const auto shape = decode(v);
        double worst = 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < points; ++i) {
            // Both sides floored, so error below the floor costs nothing: the
            // fit spends its freedom where the response is still audible
            // instead of chasing −300 dB.
            const double modeled = std::max(magnitude(shape, x[i]), kTapeLossFloorDb);
            const double error = std::abs(modeled - target_db[i]);
            worst = std::max(worst, error);
            sum += error * error;
        }
        return worst + 1e-4 * sum;
    };

    // A simplex collapses as it converges and a collapsed simplex cannot
    // explore, so each start is re-seeded from its own best point and run again.
    auto run = [&](std::array<double, kDimension> start) {
        for (int restart = 0; restart < kLossIirFitRestarts; ++restart) {
            std::array<std::array<double, kDimension>, kDimension + 1> simplex;
            std::array<double, kDimension + 1> value{};
            simplex[0] = start;
            for (std::size_t i = 0; i < kDimension; ++i) {
                const bool is_depth = i >= Poles && ((i - Poles) % 2u) == 1u;
                simplex[i + 1] = start;
                simplex[i + 1][i] += is_depth ? 3.0 : 0.25;
            }
            for (std::size_t i = 0; i <= kDimension; ++i) value[i] = objective(simplex[i]);

            auto combine = [&](const std::array<double, kDimension>& a,
                               const std::array<double, kDimension>& b, double t) {
                std::array<double, kDimension> out{};
                for (std::size_t i = 0; i < kDimension; ++i) out[i] = a[i] + t * (b[i] - a[i]);
                return out;
            };

            for (int iteration = 0; iteration < kLossIirFitIterations; ++iteration) {
                std::array<std::size_t, kDimension + 1> order{};
                std::iota(order.begin(), order.end(), 0u);
                std::sort(order.begin(), order.end(),
                          [&](std::size_t a, std::size_t b) { return value[a] < value[b]; });
                const std::size_t best = order.front();
                const std::size_t worst = order.back();
                const std::size_t second = order[kDimension - 1];

                std::array<double, kDimension> centroid{};
                for (std::size_t i = 0; i <= kDimension; ++i) {
                    if (i == worst) continue;
                    for (std::size_t k = 0; k < kDimension; ++k)
                        centroid[k] += simplex[i][k] / static_cast<double>(kDimension);
                }

                const auto reflected = combine(centroid, simplex[worst], -1.0);
                const double reflected_value = objective(reflected);
                if (reflected_value < value[best]) {
                    const auto expanded = combine(centroid, simplex[worst], -2.0);
                    const double expanded_value = objective(expanded);
                    simplex[worst] = (expanded_value < reflected_value) ? expanded : reflected;
                    value[worst] = std::min(expanded_value, reflected_value);
                } else if (reflected_value < value[second]) {
                    simplex[worst] = reflected;
                    value[worst] = reflected_value;
                } else {
                    const auto contracted = combine(centroid, simplex[worst], 0.5);
                    const double contracted_value = objective(contracted);
                    if (contracted_value < value[worst]) {
                        simplex[worst] = contracted;
                        value[worst] = contracted_value;
                    } else {
                        for (std::size_t i = 0; i <= kDimension; ++i) {
                            if (i == best) continue;
                            simplex[i] = combine(simplex[best], simplex[i], 0.5);
                            value[i] = objective(simplex[i]);
                        }
                    }
                }
            }
            std::size_t best = 0;
            for (std::size_t i = 1; i <= kDimension; ++i)
                if (value[i] < value[best]) best = i;
            start = simplex[best];
        }
        return start;
    };

    // Multi-start with a GEOMETRIC gain ladder. The target steepens
    // exponentially in log-frequency, so each successive shelf must cut roughly
    // twice what the last did; starting from equal cuts leaves the simplex in a
    // local minimum that misses the top of the band by several decibels.
    std::array<double, kDimension> best{};
    double best_value = 0.0;
    bool have_best = false;
    for (int start_index = 0; start_index < kLossIirFitStarts; ++start_index) {
        const double spread = 1.5 + 0.35 * static_cast<double>(start_index);
        const double base_db = -1.5 - 0.5 * static_cast<double>(start_index);
        // Anchor the ladder where the target crosses −3 dB.
        double anchor = kLossShapeMaxX;
        for (double f = kLossShapeMinX; f < kLossShapeMaxX; f *= 1.02) {
            if (target(f) <= -3.0) {
                anchor = f;
                break;
            }
        }
        std::array<double, kDimension> start{};
        std::size_t k = 0;
        for (std::size_t i = 0; i < Poles; ++i)
            start[k++] = std::log10(anchor * std::pow(spread, static_cast<double>(i)));
        for (std::size_t i = 0; i < Shelves; ++i) {
            start[k++] =
                std::log10(anchor * 0.5 * std::pow(spread, static_cast<double>(i)));
            start[k++] = std::max(base_db * std::pow(1.7, static_cast<double>(i)),
                                  kLossShelfMinDb);
        }
        const auto candidate = run(start);
        const double value = objective(candidate);
        if (!have_best || value < best_value) {
            best = candidate;
            best_value = value;
            have_best = true;
        }
    }
    return decode(best);
}

}  // namespace detail

/// Fit both dimensionless shapes. Allocating and iterative; control thread
/// only, and only once per instance — after this, every speed and every age is
/// a scalar multiply.
struct TapeLossShapes {
    SpacingShape spacing;
    ThicknessShape thickness;

    /// Re-derive the shapes from the physics. Expensive (a multi-start simplex
    /// search); this is the offline derivation behind the shipped table, kept so
    /// the table can be regenerated and so the acceptance suite can prove the
    /// shipped values reproduce it.
    void fit() {
        spacing = detail::fit_loss_shape<kSpacingPoleSections, kSpacingShelfSections>(
            [](double x) { return spacing_shape_db(x); });
        thickness = detail::fit_loss_shape<kThicknessPoleSections, kThicknessShelfSections>(
            [](double x) { return thickness_shape_db(x); });
    }

    /// The shipped values.
    static TapeLossShapes tabulated() noexcept {
        TapeLossShapes shapes;
        shapes.spacing.pole_x = kSpacingShapePoleX;
        shapes.spacing.shelf_x = kSpacingShapeShelfX;
        shapes.spacing.shelf_db = kSpacingShapeShelfDb;
        shapes.thickness.pole_x = kThicknessShapePoleX;
        shapes.thickness.shelf_x = kThicknessShapeShelfX;
        shapes.thickness.shelf_db = kThicknessShapeShelfDb;
        return shapes;
    }

    /// Corners in hertz for a given geometry. The spacing shape's normalized
    /// variable is f·d/v, so its corners become hertz when multiplied by v/d;
    /// the thickness shape's is f·δ/v, so by v/δ. Both are exact.
    TapeLossIirParams parameters_for(const TapeLossGeometry& geometry) const noexcept {
        const double velocity = std::max(geometry.speed_ips * 0.0254, 1e-9);
        const double spacing_scale = velocity / std::max(geometry.spacing_m, 1e-12);
        const double thickness_scale = velocity / std::max(geometry.thickness_m, 1e-12);

        TapeLossIirParams parameters;
        std::size_t pole = 0;
        std::size_t shelf = 0;
        for (std::size_t i = 0; i < kSpacingPoleSections; ++i)
            parameters.pole_hz[pole++] = spacing.pole_x[i] * spacing_scale;
        for (std::size_t i = 0; i < kSpacingShelfSections; ++i) {
            parameters.shelf_hz[shelf] = spacing.shelf_x[i] * spacing_scale;
            parameters.shelf_db[shelf++] = spacing.shelf_db[i];
        }
        for (std::size_t i = 0; i < kThicknessPoleSections; ++i)
            parameters.pole_hz[pole++] = thickness.pole_x[i] * thickness_scale;
        for (std::size_t i = 0; i < kThicknessShelfSections; ++i) {
            parameters.shelf_hz[shelf] = thickness.shelf_x[i] * thickness_scale;
            parameters.shelf_db[shelf++] = thickness.shelf_db[i];
        }
        return parameters;
    }
};

/// The process-wide fitted shapes.
///
/// Fitting is a multi-start simplex search and takes a moment; doing it once is
/// possible only because the shapes are dimensionless — they are properties of
/// the PHYSICS, not of any particular machine, sample rate or setting. Every
/// instance, at every speed and every age, is that same pair of shapes with
/// their frequency axes scaled.
inline const TapeLossShapes& tape_loss_shapes() {
    static const TapeLossShapes shapes = TapeLossShapes::tabulated();
    return shapes;
}

/// The offline derivation. Control thread, and slow — the acceptance suite uses
/// it to prove the shipped table still reproduces the physics.
inline TapeLossShapes fit_tape_loss_shapes() {
    TapeLossShapes shapes;
    shapes.fit();
    return shapes;
}

/// The realized IIR cascade for one channel.
class TapeLossIir {
public:
    void set(const TapeLossIirParams& parameters, double fs) noexcept {
        parameters_ = parameters;
        for (std::size_t i = 0; i < kLossPoleSections; ++i)
            poles_[i].set_cutoff(parameters.pole_hz[i], fs);
        for (std::size_t i = 0; i < kLossShelfSections; ++i)
            shelves_[i].set(parameters.shelf_hz[i],
                            std::pow(10.0, parameters.shelf_db[i] / 20.0), fs);
    }

    void reset() noexcept {
        for (auto& pole : poles_) pole.reset();
        for (auto& shelf : shelves_) shelf.reset();
    }

    double process(double x) noexcept {
        for (auto& pole : poles_) x = pole.lowpass(x);
        for (auto& shelf : shelves_) x = shelf.process(x);
        return x;
    }

    /// Group delay at DC, in seconds. Each one-pole contributes 1/ω and each
    /// shelf (1 - G)/ω. This sits inside the feedback loop, so the engine folds
    /// it out of the delay line rather than letting it push every repeat late.
    double dc_group_delay_seconds() const noexcept {
        double seconds = 0.0;
        for (double corner : parameters_.pole_hz)
            seconds += 1.0 / (2.0 * kPi * std::max(corner, 1e-9));
        for (std::size_t i = 0; i < kLossShelfSections; ++i) {
            const double gain = std::pow(10.0, parameters_.shelf_db[i] / 20.0);
            seconds += (1.0 - gain) / (2.0 * kPi * std::max(parameters_.shelf_hz[i], 1e-9));
        }
        return seconds;
    }

    const TapeLossIirParams& parameters() const noexcept { return parameters_; }

private:
    std::array<OnePole, kLossPoleSections> poles_{};
    std::array<FirstOrderShelf, kLossShelfSections> shelves_{};
    TapeLossIirParams parameters_;
};

// ── The gap null: a minimum-phase FIR ─────────────────────────────────────

/// FIR length at a given sample rate: the specified order at 48 kHz, scaled
/// with the rate so the filter spans the same amount of TIME on every host.
inline std::size_t tape_gap_fir_taps(double fs) {
    const auto scaled = static_cast<std::size_t>(
        std::llround(static_cast<double>(kLossFirOrder48k) * fs / 48000.0));
    return std::clamp<std::size_t>(scaled, 32u, 512u) + 1u;
}

/// Design a minimum-phase FIR realizing the GAP term only.
///
/// Minimum phase rather than linear phase because this runs inside the feedback
/// loop: a linear-phase kernel of this length would add its whole group delay
/// to every recirculation. The cepstral fold is the standard construction.
///
/// Allocating; control thread only.
inline std::vector<double> design_tape_gap_fir(double fs, std::size_t taps,
                                               const TapeLossGeometry& geometry) {
    int fft_size = 64;
    while (static_cast<std::size_t>(fft_size) < taps * 8u) fft_size <<= 1;
    const auto size = static_cast<std::size_t>(fft_size);

    FftT<double> fft(fft_size);
    std::vector<std::complex<double>> spectrum(size);

    const double floor_linear = std::pow(10.0, kTapeLossFloorDb / 20.0);
    for (std::size_t bin = 0; bin <= size / 2; ++bin) {
        const double frequency = static_cast<double>(bin) * fs / static_cast<double>(size);
        const double magnitude =
            std::max(tape_loss_gap_magnitude(frequency, geometry), floor_linear);
        const double log_magnitude = std::log(magnitude);
        spectrum[bin] = {log_magnitude, 0.0};
        if (bin > 0 && bin < size / 2) spectrum[size - bin] = {log_magnitude, 0.0};
    }

    // Real cepstrum folded to its causal part: the minimum-phase spectrum is
    // the analytic completion of the log-magnitude, and folding the cepstrum is
    // how you get it without unwrapping a phase.
    fft.inverse(spectrum.data());
    std::vector<std::complex<double>> cepstrum(size, std::complex<double>{0.0, 0.0});
    cepstrum[0] = {spectrum[0].real(), 0.0};
    for (std::size_t n = 1; n < size / 2; ++n) cepstrum[n] = {2.0 * spectrum[n].real(), 0.0};
    cepstrum[size / 2] = {spectrum[size / 2].real(), 0.0};

    fft.forward(cepstrum.data());
    for (auto& value : cepstrum) value = std::exp(value);
    fft.inverse(cepstrum.data());

    // The window is the TRAILING HALF of a Kaiser, not a symmetric one. A
    // minimum-phase impulse response concentrates its energy at the START, so a
    // symmetric window would attenuate h[0] — where nearly all of it is — to
    // nothing and amplify the tail. That is not a taper, it is a different
    // filter, and it cost 34 dB of error before it was caught.
    std::vector<double> symmetric(2u * taps - 1u, 0.0);
    kaiser_window(symmetric, kaiser_beta_for_stopband(kHysteresisHalfBandStopbandDb));

    std::vector<double> coefficients(taps, 0.0);
    for (std::size_t i = 0; i < taps; ++i)
        coefficients[i] = cepstrum[i].real() * symmetric[taps - 1u + i];

    // The gap term is unity at DC, so normalize the realized DC gain to 1 and
    // keep windowing from introducing a level shift the loop would compound.
    double sum = 0.0;
    for (double c : coefficients) sum += c;
    if (std::abs(sum) > 1e-9)
        for (double& c : coefficients) c /= sum;
    return coefficients;
}

/// Everything designed for one tape speed: the gap FIR (speed-only) and the
/// scaled IIR parameters, which are a pure function of geometry once the
/// dimensionless shapes are fitted.
class TapeLossDesign {
public:
    /// Expensive: fits both dimensionless shapes. Once per instance.
    void prepare(double fs, double speed_ips) {
        sample_rate_ = fs;
        shapes_ = &tape_loss_shapes();
        set_speed(speed_ips);
    }

    /// Cheap by comparison: only the gap FIR is redesigned, because only it
    /// depends on the speed in a way a scale factor cannot express.
    void set_speed(double speed_ips) {
        speed_ips_ = speed_ips;
        TapeLossGeometry geometry;
        geometry.speed_ips = speed_ips;
        gap_taps_ = design_tape_gap_fir(sample_rate_, tape_gap_fir_taps(sample_rate_), geometry);

        double weighted = 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < gap_taps_.size(); ++i) {
            weighted += static_cast<double>(i) * gap_taps_[i];
            sum += gap_taps_[i];
        }
        gap_group_delay_samples_ = (std::abs(sum) > 1e-12) ? weighted / sum : 0.0;
    }

    bool empty() const noexcept { return gap_taps_.empty(); }
    double speed_ips() const noexcept { return speed_ips_; }
    const std::vector<double>& gap_taps() const noexcept { return gap_taps_; }
    double gap_group_delay_samples() const noexcept { return gap_group_delay_samples_; }
    const TapeLossShapes& shapes() const noexcept { return *shapes_; }

    /// The geometry at a given age: spacing walks the age table, everything
    /// else is fixed.
    TapeLossGeometry geometry_at(double age) const noexcept {
        TapeLossGeometry geometry;
        geometry.speed_ips = speed_ips_;
        geometry.spacing_m =
            interpolate_knots(kAgeAxis, kAgeSpacingUm, std::clamp(age, 0.0, 1.0)) * 1e-6;
        return geometry;
    }

    /// Filter parameters at `age`. Exact at every age — a scalar multiply of a
    /// fitted shape, not an interpolation between fits. Allocation-free.
    TapeLossIirParams parameters_at(double age) const noexcept {
        return shapes_->parameters_for(geometry_at(age));
    }

private:
    const TapeLossShapes* shapes_ = &tape_loss_shapes();
    std::vector<double> gap_taps_;
    double gap_group_delay_samples_ = 0.0;
    double sample_rate_ = 48000.0;
    double speed_ips_ = 7.5;
};

/// One complete, independently-stateful loss filter: the gap FIR followed by
/// the fitted IIR tilt. Two of these exist per channel so a speed change can be
/// crossfaded between whole instances rather than between coefficient sets.
class TapeLossStage {
public:
    void prepare(std::size_t max_taps) { fir_.prepare(max_taps); }

    void reset() noexcept {
        fir_.reset();
        iir_.reset();
    }

    /// Reset only the recursive half. Used when a stage adopts a new design: the
    /// FIR's history is just delayed input and stays meaningful, but IIR state
    /// belongs to the coefficients that produced it.
    void reset_recursive() noexcept { iir_.reset(); }

    void set_parameters(const TapeLossIirParams& parameters, double fs) noexcept {
        iir_.set(parameters, fs);
    }

    void push(double x) noexcept { fir_.push(x); }

    double process(const TapeLossDesign& design) noexcept {
        return iir_.process(fir_.convolve(design.gap_taps().data(), design.gap_taps().size()));
    }

    /// The stage's own history, so a newly adopted design starts from the same
    /// input rather than from silence.
    void copy_history_from(const TapeLossStage& other) { fir_ = other.fir_; }

    double dc_group_delay_seconds() const noexcept { return iir_.dc_group_delay_seconds(); }
    const TapeLossIirParams& parameters() const noexcept { return iir_.parameters(); }

private:
    FixedFir fir_;
    TapeLossIir iir_;
};

}  // namespace pulp::signal::chardelay
