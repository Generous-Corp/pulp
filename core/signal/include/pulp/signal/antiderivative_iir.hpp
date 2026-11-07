#pragma once

/// @file antiderivative_iir.hpp
/// Arbitrary-order IIR antiderivative antialiasing for static nonlinearities.
///
/// A memoryless nonlinearity `f` applied sample by sample creates harmonics
/// that fold back below Nyquist. Antiderivative antialiasing evaluates what an
/// analog chain would do instead: interpolate the input linearly between
/// samples, apply `f` in continuous time, low-pass the result with an
/// antialiasing kernel `h`, and sample it. With a finite rectangular kernel
/// that is first-order ADAA. Here the kernel is any causal rational transfer
/// function with simple poles, which allows a much steeper cut than a
/// rectangle at a cost that grows linearly with the filter order.
///
/// Writing `H(s) = A0 + sum_k A_k / (s - p_k)` (partial fractions; complex
/// poles in conjugate pairs), each pole's contribution obeys a one-step
/// recursion. For one pole `p` and the interval between inputs `x_n` and
/// `x_{n+1}`, with `D = x_{n+1} - x_n`:
///
/// ```
///   yhat_{n+1} = e^p yhat_n + A * I,
///   I = integral over t in [0,1] of  f(x_n + t D) e^{p (1 - t)} dt
/// ```
///
/// and the output is the sum over poles of `yhat` (the real part for a complex
/// pole, whose weight is `2 B` for the residue `B` of the pole in the upper
/// half plane). Writing the integral over the unit parameter `t` instead of
/// over the input value removes the division by `D`, so a stalled or reversing
/// input needs no special case and nothing becomes ill conditioned.
///
/// How `I` is evaluated depends on the nonlinearity:
///  - piecewise-linear curves (hard clip) use the exact closed form on each
///    linear piece;
///  - smooth curves (tanh, atan, asinh, the cubic's pieces) use composite
///    8-point Gauss-Legendre quadrature, with enough panels to resolve the
///    curve over the span the input crosses in one sample;
///  - kinks (points where the curve is not smooth) are reported by the
///    nonlinearity and split the interval, so no quadrature straddles one.
///
/// Delay: the kernel's low-frequency group delay (`group_delay_samples()`,
/// about 1.4 samples for the default), because the kernel is causal.
///
/// Requirements on the nonlinearity: `f(0) = 0` (subtract `f(0)` and add it
/// back otherwise). Poles must be simple; repeated poles are not supported.
///
/// Reference: Pier Paolo La Pastina, Stefano D'Angelo and Leonardo Gabrielli,
/// "Arbitrary-Order IIR Antiderivative Antialiasing", Proceedings of the 24th
/// International Conference on Digital Audio Effects (DAFx), 2021, pp. 9-16.
/// First-order antiderivative antialiasing: Julian Parker, Vadim Zavalishin
/// and Efflam Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using
/// Continuous-Time Convolution", DAFx 2016.
///
/// RT contract: kernel design is a control-thread operation (fixed-size
/// storage, no allocation). `process()`, `reseed()` and `reset()` allocate
/// nothing and do bounded work: at most `kMaxPanels` quadrature panels of
/// eight nodes per kink-free span, with the nonlinearity evaluated once per
/// node.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>

namespace pulp::signal {

/// `tanh(x)`. Smooth; features are about one unit wide.
struct TanhNonlinearity {
    static constexpr bool piecewise_linear = false;
    double operator()(double x) const noexcept {
        return std::tanh(x);
    }
    double length_scale() const noexcept {
        return 1.0;
    }
    int breakpoints(double, double, double*) const noexcept {
        return 0;
    }
};

/// Hard clip to `[-threshold, threshold]`. Piecewise linear, with kinks at
/// the two thresholds.
struct HardClipNonlinearity {
    static constexpr bool piecewise_linear = true;
    double threshold = 1.0;
    double operator()(double x) const noexcept {
        return std::clamp(x, -threshold, threshold);
    }
    double length_scale() const noexcept {
        return threshold;
    }
    int breakpoints(double lo, double hi, double* out) const noexcept {
        int n = 0;
        if (-threshold > lo && -threshold < hi)
            out[n++] = -threshold;
        if (threshold > lo && threshold < hi)
            out[n++] = threshold;
        return n;
    }
};

/// Design of the antialiasing kernel: poles and partial-fraction weights of a
/// causal low-pass transfer function with unit gain at DC, in radians per
/// sample (sample rate 1).
class AntiderivativeIirKernel {
  public:
    static constexpr std::size_t kMaxSections = 8;
    static constexpr int kMaxButterworthOrder = 8;

    struct Section {
        bool pair = false;                    ///< conjugate pair (true) or one real pole
        std::complex<double> pole{};          ///< `p` (upper half plane for a pair)
        std::complex<double> weight{};        ///< `A` (real pole) or `2 B` (pair)
        std::complex<double> decay{};         ///< `e^p`, one sample of decay
        std::complex<double> unit_integral{}; ///< `integral_0^1 e^{p(1-t)} dt`
        std::complex<double> ramp_integral{}; ///< `integral_0^1 t e^{p(1-t)} dt`
    };

    /// Butterworth low-pass of `order` (1 to 8) with the -3 dB frequency at
    /// `cutoff_ratio` times the sample rate (0.01 to 0.49). Poles
    /// `p_k = w_c exp(j pi (2k + n + 1) / 2n)`, residues
    /// `A_k = w_c^n / prod_{j != k} (p_k - p_j)`.
    static AntiderivativeIirKernel butterworth(int order, double cutoff_ratio) {
        order = std::clamp(order, 1, kMaxButterworthOrder);
        cutoff_ratio = std::clamp(cutoff_ratio, 0.01, 0.49);
        const double wc = 2.0 * kPi * cutoff_ratio;
        std::array<std::complex<double>, kMaxButterworthOrder> poles{};
        for (int k = 0; k < order; ++k) {
            const double angle = kPi * (2.0 * k + order + 1.0) / (2.0 * order);
            poles[static_cast<std::size_t>(k)] = std::polar(wc, angle);
        }
        AntiderivativeIirKernel kernel;
        for (int k = 0; k < order; ++k) {
            const auto pk = poles[static_cast<std::size_t>(k)];
            std::complex<double> denominator = 1.0;
            for (int j = 0; j < order; ++j)
                if (j != k)
                    denominator *= pk - poles[static_cast<std::size_t>(j)];
            const std::complex<double> residue = std::pow(wc, order) / denominator;
            if (std::abs(pk.imag()) < 1e-12 * wc)
                kernel.add_real(pk.real(), residue.real());
            else if (pk.imag() > 0.0)
                kernel.add_pair(pk, residue);
        }
        return kernel;
    }

    /// Kernel from explicit simple poles and residues. A real pole `p` with
    /// residue `A`; for a complex pair give the pole in the upper half plane
    /// and its residue (the conjugate pole is implied). Poles must have a
    /// negative real part. Returns false if capacity is exceeded or a pole is
    /// unstable.
    bool add_real(double pole, double residue) {
        if (count_ >= kMaxSections || !(pole < 0.0))
            return false;
        Section s;
        s.pair = false;
        s.pole = {pole, 0.0};
        s.weight = {residue, 0.0};
        finish(s);
        sections_[count_++] = s;
        return true;
    }
    bool add_pair(std::complex<double> pole, std::complex<double> residue) {
        if (count_ >= kMaxSections || !(pole.real() < 0.0) || !(pole.imag() > 0.0))
            return false;
        Section s;
        s.pair = true;
        s.pole = pole;
        s.weight = 2.0 * residue;
        finish(s);
        sections_[count_++] = s;
        return true;
    }

    std::size_t size() const noexcept {
        return count_;
    }
    const Section& section(std::size_t i) const noexcept {
        return sections_[i];
    }

    /// `|H(j 2 pi f)|` for `f` in cycles per sample. A conjugate pair with
    /// residue `B` contributes `B / (jw - p) + conj(B) / (jw - conj(p))`.
    double magnitude(double cycles_per_sample) const {
        const std::complex<double> jw{0.0, 2.0 * kPi * cycles_per_sample};
        std::complex<double> h = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& s = sections_[i];
            if (s.pair) {
                const std::complex<double> residue = 0.5 * s.weight;
                h += residue / (jw - s.pole) + std::conj(residue) / (jw - std::conj(s.pole));
            } else {
                h += s.weight / (jw - s.pole);
            }
        }
        return std::abs(h);
    }

    /// Low-frequency group delay in samples: the sum over poles of
    /// `-Re(p) / |p|^2` (both members of a pair).
    double group_delay_samples() const noexcept {
        double delay = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& s = sections_[i];
            const double per_pole = -s.pole.real() / std::norm(s.pole);
            delay += s.pair ? 2.0 * per_pole : per_pole;
        }
        return delay;
    }

    /// Steady-state value of `yhat` for a constant nonlinearity output `c`:
    /// `-weight * c / pole`.
    std::complex<double> steady_state(std::size_t i, double c) const noexcept {
        return -sections_[i].weight * c / sections_[i].pole;
    }

    static constexpr double kPi = 3.14159265358979323846;

  private:
    static void finish(Section& s) {
        s.decay = std::exp(s.pole);
        s.unit_integral = (s.decay - 1.0) / s.pole;
        s.ramp_integral = -1.0 / s.pole + s.unit_integral / s.pole;
    }

    std::array<Section, kMaxSections> sections_{};
    std::size_t count_ = 0;
};

/// The per-sample processor: holds the kernel, the previous input and one
/// complex state per pole.
class AntiderivativeIir {
  public:
    /// Quadrature panels per kink-free span at most; beyond it the curve is
    /// treated as resolved at that density.
    static constexpr int kMaxPanels = 16;
    /// Input change per panel, in units of the curve's length scale.
    static constexpr double kPanelSpan = 2.0;

    /// The default kernel: fourth-order Butterworth, -3 dB at 0.3 of the sample
    /// rate. Measured on a heavily driven 1 kHz sine at 44.1 kHz it sits within
    /// a dB or two of the best Butterworth order and cutoff for both tanh and
    /// hard clip, with a group delay of about 1.4 samples.
    static constexpr double kDefaultCutoffRatio = 0.30;

    AntiderivativeIir() {
        set_kernel(AntiderivativeIirKernel::butterworth(4, kDefaultCutoffRatio));
    }
    explicit AntiderivativeIir(const AntiderivativeIirKernel& kernel) {
        set_kernel(kernel);
    }

    void set_kernel(const AntiderivativeIirKernel& kernel) {
        kernel_ = kernel;
        for (std::size_t i = 0; i < kernel_.size(); ++i) {
            const auto& s = kernel_.section(i);
            for (int j = 0; j < kNodes; ++j) {
                const double t = kNodeT[j];
                node_kernel_[i][static_cast<std::size_t>(j)] =
                    kNodeW[j] * std::exp(s.pole * (1.0 - t));
            }
        }
        reset();
    }
    const AntiderivativeIirKernel& kernel() const noexcept {
        return kernel_;
    }

    void reset() noexcept {
        previous_ = 0.0;
        state_.fill(std::complex<double>{});
    }

    /// Start from the steady state of a constant input `x` whose curve value
    /// is `fx` (so a mid-stream start does not ring).
    void reseed(double x, double fx) noexcept {
        previous_ = x;
        for (std::size_t i = 0; i < kernel_.size(); ++i)
            state_[i] = kernel_.steady_state(i, fx);
    }

    double previous_input() const noexcept {
        return previous_;
    }

    /// One sample through `nonlinearity`. A non-finite input resets the state
    /// and returns 0.
    template <class Nonlinearity> double process(const Nonlinearity& f, double x) {
        if (!std::isfinite(x)) {
            reset();
            return 0.0;
        }
        const double x0 = previous_;
        const double delta = x - x0;
        previous_ = x;

        // Split points (in the unit parameter) at the curve's kinks.
        std::array<double, kMaxSplits + 2> knots{};
        int knot_count = 0;
        knots[static_cast<std::size_t>(knot_count++)] = 0.0;
        if (delta != 0.0) {
            std::array<double, kMaxSplits> kinks{};
            const int found =
                std::min(kMaxSplits, f.breakpoints(std::min(x0, x), std::max(x0, x), kinks.data()));
            std::array<double, kMaxSplits> ts{};
            for (int i = 0; i < found; ++i)
                ts[static_cast<std::size_t>(i)] = (kinks[static_cast<std::size_t>(i)] - x0) / delta;
            std::sort(ts.begin(), ts.begin() + found);
            for (int i = 0; i < found; ++i) {
                const double t = ts[static_cast<std::size_t>(i)];
                if (t > knots[static_cast<std::size_t>(knot_count - 1)] && t < 1.0)
                    knots[static_cast<std::size_t>(knot_count++)] = t;
            }
        }
        knots[static_cast<std::size_t>(knot_count++)] = 1.0;

        std::array<std::complex<double>, AntiderivativeIirKernel::kMaxSections> integral{};
        const std::size_t sections = kernel_.size();

        if (knot_count == 2) {
            if constexpr (Nonlinearity::piecewise_linear) {
                const double f0 = f(x0), f1 = f(x);
                for (std::size_t i = 0; i < sections; ++i) {
                    const auto& s = kernel_.section(i);
                    integral[i] = f0 * s.unit_integral + (f1 - f0) * s.ramp_integral;
                }
            } else if (std::abs(delta) <= kPanelSpan * f.length_scale()) {
                // One panel over the whole interval: the cached kernel weights.
                std::array<double, kNodes> fv{};
                for (int j = 0; j < kNodes; ++j)
                    fv[static_cast<std::size_t>(j)] = f(x0 + kNodeT[j] * delta);
                for (std::size_t i = 0; i < sections; ++i) {
                    std::complex<double> acc = 0.0;
                    for (int j = 0; j < kNodes; ++j)
                        acc += fv[static_cast<std::size_t>(j)] *
                               node_kernel_[i][static_cast<std::size_t>(j)];
                    integral[i] = acc;
                }
            } else {
                add_smooth_span(f, x0, delta, 0.0, 1.0, integral);
            }
        } else {
            for (int k = 0; k + 1 < knot_count; ++k) {
                const double ta = knots[static_cast<std::size_t>(k)];
                const double tb = knots[static_cast<std::size_t>(k + 1)];
                if constexpr (Nonlinearity::piecewise_linear)
                    add_linear_span(f, x0, delta, ta, tb, integral);
                else
                    add_smooth_span(f, x0, delta, ta, tb, integral);
            }
        }

        double y = 0.0;
        for (std::size_t i = 0; i < sections; ++i) {
            const auto& s = kernel_.section(i);
            state_[i] = s.decay * state_[i] + s.weight * integral[i];
            y += state_[i].real();
        }
        return y;
    }

  private:
    static constexpr int kNodes = 8;
    static constexpr int kMaxSplits = 4;
    // Gauss-Legendre nodes and weights on [0, 1], eight points.
    static constexpr double kNodeT[kNodes] = {
        0.0198550717512319, 0.1016667612931866, 0.2372337950418355, 0.4082826787521751,
        0.5917173212478249, 0.7627662049581645, 0.8983332387068134, 0.9801449282487681};
    static constexpr double kNodeW[kNodes] = {
        0.0506142681451881, 0.1111905172266872, 0.1568533229389436, 0.1813418916891810,
        0.1813418916891810, 0.1568533229389436, 0.1111905172266872, 0.0506142681451881};

    using Integral = std::array<std::complex<double>, AntiderivativeIirKernel::kMaxSections>;

    /// Exact integral of a curve that is linear on `[ta, tb]`.
    template <class Nonlinearity>
    void add_linear_span(const Nonlinearity& f, double x0, double delta, double ta, double tb,
                         Integral& integral) const {
        const double fa = f(x0 + ta * delta), fb = f(x0 + tb * delta);
        const double h = tb - ta;
        for (std::size_t i = 0; i < kernel_.size(); ++i) {
            const auto& s = kernel_.section(i);
            const std::complex<double> ea = std::exp(s.pole * (1.0 - ta));
            const std::complex<double> eb = std::exp(s.pole * (1.0 - tb));
            std::complex<double> value;
            if (h >= 1e-3) {
                const std::complex<double> j0 = (ea - eb) / s.pole;
                const std::complex<double> slope = -eb / s.pole + j0 / (s.pole * h);
                value = fa * j0 + (fb - fa) * slope;
            } else {
                // A sliver: Simpson on the (exactly linear) integrand.
                const std::complex<double> em = std::exp(s.pole * (1.0 - 0.5 * (ta + tb)));
                const double fm = 0.5 * (fa + fb);
                value = h / 6.0 * (fa * ea + 4.0 * fm * em + fb * eb);
            }
            integral[i] += value;
        }
    }

    /// Composite Gauss-Legendre over a smooth span `[ta, tb]`.
    template <class Nonlinearity>
    void add_smooth_span(const Nonlinearity& f, double x0, double delta, double ta, double tb,
                         Integral& integral) const {
        const double span = std::abs(delta) * (tb - ta);
        // Clamp before converting: a huge span must not overflow the int.
        const int panels =
            static_cast<int>(std::clamp(std::ceil(span / (kPanelSpan * f.length_scale())), 1.0,
                                        static_cast<double>(kMaxPanels)));
        const double h = (tb - ta) / panels;
        for (int p = 0; p < panels; ++p) {
            const double pa = ta + h * p;
            for (int j = 0; j < kNodes; ++j) {
                const double t = pa + h * kNodeT[j];
                const double fv = f(x0 + t * delta) * kNodeW[j] * h;
                for (std::size_t i = 0; i < kernel_.size(); ++i)
                    integral[i] += fv * std::exp(kernel_.section(i).pole * (1.0 - t));
            }
        }
    }

    AntiderivativeIirKernel kernel_{};
    std::array<std::array<std::complex<double>, kNodes>, AntiderivativeIirKernel::kMaxSections>
        node_kernel_{};
    std::array<std::complex<double>, AntiderivativeIirKernel::kMaxSections> state_{};
    double previous_ = 0.0;
};

} // namespace pulp::signal
