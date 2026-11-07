// Arbitrary-order IIR antiderivative antialiasing: the kernel design, the
// quadrature and kink handling against a brute-force reference, the aliasing
// floor against first- and second-order antiderivative antialiasing and the
// naive curve, and the Saturator policy built on it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/antiderivative_iir.hpp>
#include <pulp/signal/saturator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace pulp::signal;

namespace {

constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

void fft(std::vector<cd>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(len);
        const cd wl(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            cd w = 1.0;
            for (std::size_t j = 0; j < len / 2; ++j) {
                const cd u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Coherent capture: the sine completes `cycles` periods in `n` samples, so
// every harmonic of it lies exactly on a multiple of `cycles` bins.
constexpr int kN = 65536;
constexpr int kCycles = 1486; // 999.9 Hz at 44.1 kHz
constexpr double kFs = 44100.0;

double alias_db(const std::vector<double>& y) {
    std::vector<cd> a(y.begin(), y.end());
    fft(a);
    double total = 0.0, alias = 0.0;
    for (int k = 1; k < kN / 2; ++k) {
        const double e = std::norm(a[static_cast<std::size_t>(k)]);
        total += e;
        if (k % kCycles != 0 && k * kFs / kN <= 20000.0)
            alias += e;
    }
    return 10.0 * std::log10(alias / total);
}

std::vector<double> sine(double amplitude) {
    std::vector<double> x(kN);
    for (int n = 0; n < kN; ++n)
        x[static_cast<std::size_t>(n)] = amplitude * std::sin(2.0 * kPi * kCycles * n / kN);
    return x;
}

// Reference for one step: the same recursion with the integral evaluated by
// brute-force composite Simpson over 20000 points, independent of the
// quadrature, closed forms and kink logic under test.
template <class F>
std::vector<double> reference(const AntiderivativeIirKernel& kernel, const F& f,
                              const std::vector<double>& x) {
    std::vector<cd> state(kernel.size());
    std::vector<double> y;
    double previous = 0.0;
    for (const double xn : x) {
        const double delta = xn - previous;
        double out = 0.0;
        for (std::size_t i = 0; i < kernel.size(); ++i) {
            const auto& s = kernel.section(i);
            const int steps = 20000;
            cd integral = 0.0;
            for (int j = 0; j <= steps; ++j) {
                const double t = static_cast<double>(j) / steps;
                const double w = (j == 0 || j == steps) ? 1.0 : (j % 2 ? 4.0 : 2.0);
                integral += w * f(previous + t * delta) * std::exp(s.pole * (1.0 - t));
            }
            integral *= 1.0 / (3.0 * steps);
            state[i] = s.decay * state[i] + s.weight * integral;
            out += state[i].real();
        }
        y.push_back(out);
        previous = xn;
    }
    return y;
}

} // namespace

TEST_CASE("Antiderivative IIR kernel is a unit-gain Butterworth low-pass of every order",
          "[signal][adaa-iir][kernel]") {
    for (int order = 1; order <= AntiderivativeIirKernel::kMaxButterworthOrder; ++order) {
        for (const double fc : {0.1, 0.3, 0.45}) {
            const auto kernel = AntiderivativeIirKernel::butterworth(order, fc);
            INFO("order " << order << " fc " << fc);
            CHECK_THAT(kernel.magnitude(0.0), WithinAbs(1.0, 1e-9));
            CHECK_THAT(kernel.magnitude(fc), WithinRel(1.0 / std::sqrt(2.0), 1e-9));
            for (const double ratio : {0.3, 0.7, 1.5, 2.0}) {
                const double want = 1.0 / std::sqrt(1.0 + std::pow(ratio, 2.0 * order));
                CHECK_THAT(kernel.magnitude(ratio * fc), WithinRel(want, 1e-7));
            }
        }
    }
    const double wc = 2.0 * kPi * 0.3;
    CHECK_THAT(AntiderivativeIirKernel::butterworth(1, 0.3).group_delay_samples(),
               WithinRel(1.0 / wc, 1e-9));
    CHECK_THAT(AntiderivativeIirKernel::butterworth(2, 0.3).group_delay_samples(),
               WithinRel(std::sqrt(2.0) / wc, 1e-9));
}

TEST_CASE("Antiderivative IIR matches a brute-force reference across kinks",
          "[signal][adaa-iir][reference]") {
    // Large steps cross the clip thresholds repeatedly, land exactly on them,
    // stall (zero change) and reverse.
    const std::vector<double> x = {0.0, 0.4, 1.0,  1.0, 1.0,  3.0,   -2.5, -1.0,       -1.0, 0.0,
                                   0.7, 0.7, -0.7, 2.0, 50.0, -50.0, 0.3,  0.30000001, 5.0,  0.0};
    for (const int order : {1, 2, 4}) {
        const auto kernel = AntiderivativeIirKernel::butterworth(order, 0.3);
        {
            AntiderivativeIir iir(kernel);
            const auto want = reference(kernel, HardClipNonlinearity{}, x);
            for (std::size_t n = 0; n < x.size(); ++n)
                CHECK_THAT(iir.process(HardClipNonlinearity{}, x[n]), WithinAbs(want[n], 2e-6));
        }
        {
            AntiderivativeIir iir(kernel);
            const auto want = reference(kernel, TanhNonlinearity{}, x);
            for (std::size_t n = 0; n < x.size(); ++n) {
                INFO("order " << order << " n " << n);
                CHECK_THAT(iir.process(TanhNonlinearity{}, x[n]), WithinAbs(want[n], 2e-6));
            }
        }
    }
}

TEST_CASE("Antiderivative IIR reproduces the naive curve for a slow signal",
          "[signal][adaa-iir][accuracy]") {
    const auto kernel = AntiderivativeIirKernel::butterworth(4, 0.3);
    const double delay = kernel.group_delay_samples();
    const double f0 = 50.0;
    AntiderivativeIir tanh_iir(kernel), clip_iir(kernel);
    const int n_total = 4410;
    double worst_tanh = 0.0, worst_clip = 0.0;
    for (int n = 0; n < n_total; ++n) {
        const double x = 3.0 * std::sin(2.0 * kPi * f0 * n / kFs);
        const double yt = tanh_iir.process(TanhNonlinearity{}, x);
        const double yc = clip_iir.process(HardClipNonlinearity{}, x);
        if (n < 40)
            continue;
        const double delayed = 3.0 * std::sin(2.0 * kPi * f0 * (n - delay) / kFs);
        worst_tanh = std::max(worst_tanh, std::abs(yt - std::tanh(delayed)));
        worst_clip = std::max(worst_clip, std::abs(yc - std::clamp(delayed, -1.0, 1.0)));
    }
    // The residual is the nonlinearity bending within one kernel length, not a
    // method error; it is small for a 50 Hz tone at 44.1 kHz.
    CHECK(worst_tanh < 2e-3);
    CHECK(worst_clip < 6e-3);
}

TEST_CASE("Antiderivative IIR settles to the curve value on constant input",
          "[signal][adaa-iir][steady-state]") {
    AntiderivativeIir iir;
    double y = 0.0;
    for (int n = 0; n < 200; ++n)
        y = iir.process(TanhNonlinearity{}, 0.8);
    CHECK_THAT(y, WithinAbs(std::tanh(0.8), 1e-9));
    for (int n = 0; n < 200; ++n)
        y = iir.process(HardClipNonlinearity{}, 7.0);
    // The kernel still remembers the tanh history; after enough hard-clip
    // samples it settles on the clipped level.
    CHECK_THAT(y, WithinAbs(1.0, 1e-6));
    // reseed() starts a mid-stream signal at its steady state.
    AntiderivativeIir seeded;
    seeded.reseed(0.5, std::tanh(0.5));
    CHECK_THAT(seeded.process(TanhNonlinearity{}, 0.5), WithinAbs(std::tanh(0.5), 1e-9));
}

TEST_CASE("Antiderivative IIR aliases less than ADAA2 and far less than the naive curve",
          "[signal][adaa-iir][alias]") {
    for (const double amplitude : {30.0, 100.0}) {
        INFO("amplitude " << amplitude);
        const auto x = sine(1.0);

        std::vector<double> naive(kN), adaa2(kN), iir(kN);
        for (int n = 0; n < kN; ++n)
            naive[static_cast<std::size_t>(n)] =
                std::tanh(amplitude * x[static_cast<std::size_t>(n)]) / amplitude;

        Saturator64 sat2;
        sat2.prepare(kFs);
        sat2.set_alias_policy(SaturatorAliasPolicy::adaa2);
        sat2.set_drive_db(20.0 * std::log10(amplitude));
        Saturator64 sat_iir;
        sat_iir.prepare(kFs);
        sat_iir.set_alias_policy(SaturatorAliasPolicy::adaa_iir);
        sat_iir.set_drive_db(20.0 * std::log10(amplitude));
        for (int n = 0; n < kN; ++n) {
            adaa2[static_cast<std::size_t>(n)] = sat2.process(x[static_cast<std::size_t>(n)]);
            iir[static_cast<std::size_t>(n)] = sat_iir.process(x[static_cast<std::size_t>(n)]);
        }
        const double naive_db = alias_db(naive);
        const double adaa2_db = alias_db(adaa2);
        const double iir_db = alias_db(iir);
        INFO("naive " << naive_db << " adaa2 " << adaa2_db << " iir " << iir_db);
        CHECK(iir_db < adaa2_db - 3.0);
        CHECK(iir_db < naive_db - 15.0);
        // The alias advantage must not come from lowpassing the passband: the
        // kernel stays within 0.3 dB of flat up to 0.2 of the sample rate.
        CHECK(20.0 * std::log10(sat_iir.adaa_iir_kernel().magnitude(0.2)) > -0.3);
    }
}

TEST_CASE("Antiderivative IIR default kernel has the documented response",
          "[signal][adaa-iir][kernel][droop]") {
    // The figures the saturator documentation quotes, against the two-sample
    // triangle of second-order antiderivative antialiasing (sinc squared).
    const AntiderivativeIir iir;
    auto db = [&](double f) { return 20.0 * std::log10(iir.kernel().magnitude(f)); };
    auto triangle_db = [](double f) {
        const double s = std::sin(kPi * f) / (kPi * f);
        return 20.0 * std::log10(s * s);
    };
    CHECK_THAT(db(0.2), WithinAbs(-0.17, 0.02));
    CHECK_THAT(db(0.25), WithinAbs(-0.91, 0.02));
    CHECK_THAT(db(0.3), WithinAbs(-3.01, 0.02));
    CHECK_THAT(db(0.4), WithinAbs(-10.4, 0.05));
    CHECK_THAT(db(0.5), WithinAbs(-17.8, 0.05));
    CHECK_THAT(triangle_db(0.2), WithinAbs(-1.16, 0.02));
    CHECK_THAT(triangle_db(0.4), WithinAbs(-4.84, 0.02));
    // Flatter than the triangle below a quarter of the rate, steeper above.
    for (const double f : {0.05, 0.1, 0.15, 0.2, 0.24})
        CHECK(db(f) > triangle_db(f));
    for (const double f : {0.3, 0.35, 0.4, 0.45, 0.5})
        CHECK(db(f) < triangle_db(f));
}

namespace {

// A piecewise-linear staircase with six kinks in [-3, 3]. It records the
// capacity it was given, writes at most that many kinks, and (if asked)
// returns a count larger than it wrote, as a miscounting user curve might.
struct ManyKinksNonlinearity {
    static constexpr bool piecewise_linear = true;
    int* seen_capacity = nullptr;
    int claimed_extra = 0;
    double operator()(double x) const noexcept {
        // Slope alternates 1 / 0.5 between kinks at -3, -2, -1, 1, 2, 3.
        const double a = std::abs(x);
        double y = std::min(a, 1.0);
        y += 0.5 * std::clamp(a - 1.0, 0.0, 1.0);
        y += 1.0 * std::clamp(a - 2.0, 0.0, 1.0);
        y += 0.25 * std::max(a - 3.0, 0.0);
        return x < 0.0 ? -y : y;
    }
    double length_scale() const noexcept {
        return 1.0;
    }
    int breakpoints(double lo, double hi, double* out, int capacity) const noexcept {
        if (seen_capacity)
            *seen_capacity = capacity;
        int n = 0;
        for (const double k : {-3.0, -2.0, -1.0, 1.0, 2.0, 3.0})
            if (n < capacity && k > lo && k < hi)
                out[n++] = k;
        return n + claimed_extra;
    }
};

} // namespace

// Measured worst case 0.074 for the steps below.
constexpr double kStraddleTolerance = 0.1;

TEST_CASE("Antiderivative IIR passes a capacity to breakpoints and truncates extra kinks",
          "[signal][adaa-iir][kink][capacity]") {
    // A buffer with guard cells on both sides: a curve that honours the
    // capacity never touches them.
    std::array<double, 2 + AntiderivativeIir::kMaxSplits + 2> guarded{};
    guarded.fill(-123.0);
    int seen = -1;
    const ManyKinksNonlinearity curve{&seen, 0};
    const int wrote = curve.breakpoints(-10.0, 10.0, guarded.data() + 2, AntiderivativeIir::kMaxSplits);
    CHECK(wrote == AntiderivativeIir::kMaxSplits);
    CHECK(guarded[0] == -123.0);
    CHECK(guarded[1] == -123.0);
    CHECK(guarded[2 + AntiderivativeIir::kMaxSplits] == -123.0);
    CHECK(guarded[3 + AntiderivativeIir::kMaxSplits] == -123.0);

    // The shipped hard clip honours a capacity of one.
    double one[1] = {0.0};
    CHECK(HardClipNonlinearity{}.breakpoints(-5.0, 5.0, one, 1) == 1);
    CHECK(one[0] == -1.0);
    CHECK(HardClipNonlinearity{}.breakpoints(-5.0, 5.0, one, 0) == 0);

    // In process(), a step across all six kinks is split at the first four;
    // the call sees the processor's capacity, and the output stays bounded by
    // the curve (a kernel with unit DC gain and a monotone curve) even when the
    // curve over-reports its count.
    const auto kernel = AntiderivativeIirKernel::butterworth(4, 0.3);
    const std::vector<double> x = {0.0, 6.0, -6.0, 6.0, 0.5, -4.0, 4.0, 0.0, 0.0, 0.0};
    const auto want = reference(kernel, ManyKinksNonlinearity{}, x);
    AntiderivativeIir honest(kernel), overclaiming(kernel);
    const ManyKinksNonlinearity c_honest{&seen, 0}, c_over{&seen, 7};
    for (std::size_t n = 0; n < x.size(); ++n) {
        INFO("n " << n);
        seen = -1;
        const double y = honest.process(c_honest, x[n]);
        CHECK((x[n] == (n ? x[n - 1] : 0.0) || seen == AntiderivativeIir::kMaxSplits));
        const double z = overclaiming.process(c_over, x[n]);
        REQUIRE(std::isfinite(y));
        // An over-reported count is clamped to the capacity; the unwritten
        // slots are zero, which at worst adds a split where the curve is
        // already split or linear, so the result matches to rounding.
        CHECK_THAT(z, WithinAbs(y, 1e-12));
        // Kinks past the capacity are straddled by the closed form, so the
        // result is close to, not exactly, the reference.
        CHECK_THAT(y, WithinAbs(want[n], kStraddleTolerance));
    }
}

TEST_CASE("Antiderivative IIR aliases less than first-order ADAA for a hard clip",
          "[signal][adaa-iir][alias][kink]") {
    const double amplitude = 30.0;
    const auto x = sine(amplitude);
    std::vector<double> naive(kN), adaa1(kN), iir(kN);
    AntiderivativeIir filter;
    const HardClipNonlinearity clip;
    // First-order antiderivative of the clip: x^2/2 inside, |x| - 1/2 outside.
    auto f1 = [](double v) { return std::abs(v) <= 1.0 ? 0.5 * v * v : std::abs(v) - 0.5; };
    double previous = 0.0;
    for (int n = 0; n < kN; ++n) {
        const double v = x[static_cast<std::size_t>(n)];
        naive[static_cast<std::size_t>(n)] = clip(v);
        const double d = v - previous;
        adaa1[static_cast<std::size_t>(n)] =
            std::abs(d) > 1e-9 ? (f1(v) - f1(previous)) / d : clip(0.5 * (v + previous));
        previous = v;
        iir[static_cast<std::size_t>(n)] = filter.process(clip, v);
    }
    const double naive_db = alias_db(naive), adaa1_db = alias_db(adaa1), iir_db = alias_db(iir);
    INFO("naive " << naive_db << " adaa1 " << adaa1_db << " iir " << iir_db);
    CHECK(iir_db < adaa1_db - 3.0);
    CHECK(iir_db < naive_db - 10.0);
}

TEST_CASE("Saturator adaa_iir policy reports its delay and stays aligned with the dry path",
          "[signal][adaa-iir][saturator]") {
    Saturator64 sat;
    sat.prepare(kFs);
    sat.set_alias_policy(SaturatorAliasPolicy::adaa_iir);
    const double delay = sat.adaa_iir_kernel().group_delay_samples();
    CHECK_THAT(sat.group_delay_samples(), WithinAbs(delay, 1e-12));
    CHECK(sat.latency_samples() == static_cast<int>(delay));

    // A level well below the knee passes almost linearly, so a dry/wet mix of
    // the wet path with a latency-matched dry path must not comb: the mix of a
    // slow sine stays close to the sine delayed by the reported latency.
    sat.set_drive_db(-12.0);
    sat.set_mix(0.5);
    const double f0 = 100.0;
    double worst = 0.0;
    for (int n = 0; n < 4410; ++n) {
        const double y = sat.process(0.1 * std::sin(2.0 * kPi * f0 * n / kFs));
        if (n < 64)
            continue;
        // Wet is delayed by `delay`, dry by the integer latency: the two agree to
        // the fractional remainder of the delay at 100 Hz.
        const double expect =
            0.1 * std::sin(2.0 * kPi * f0 * (n - 0.5 * (delay + sat.latency_samples())) / kFs);
        worst = std::max(worst, std::abs(y - expect));
    }
    CHECK(worst < 2e-3);
}

TEST_CASE("Saturator switches to adaa_iir without a step and accepts a custom kernel",
          "[signal][adaa-iir][saturator]") {
    Saturator64 sat;
    sat.prepare(kFs);
    sat.set_drive_db(12.0);
    double last = 0.0;
    for (int n = 0; n < 100; ++n)
        last = sat.process(0.3);
    const double settled = last;
    sat.set_alias_policy(SaturatorAliasPolicy::adaa_iir);
    CHECK_THAT(sat.process(0.3), WithinAbs(settled, 1e-9));

    CHECK(sat.set_adaa_iir_kernel(AntiderivativeIirKernel::butterworth(6, 0.35)));
    CHECK(sat.adaa_iir_kernel().size() == 3);
    // An empty kernel, or one whose delay exceeds the dry-path ring, is
    // refused and the current kernel stays.
    CHECK_FALSE(sat.set_adaa_iir_kernel(AntiderivativeIirKernel{}));
    CHECK_FALSE(sat.set_adaa_iir_kernel(AntiderivativeIirKernel::butterworth(8, 0.01)));
    CHECK(sat.adaa_iir_kernel().size() == 3);
    double y = 0.0;
    for (int n = 0; n < 300; ++n)
        y = sat.process(0.3);
    CHECK_THAT(y, WithinAbs(settled, 1e-9));
}

TEST_CASE("Saturator adaa_iir handles the cubic clip boundary and bias",
          "[signal][adaa-iir][saturator][kink]") {
    for (const double bias : {0.0, 0.4, -0.7}) {
        Saturator64 sat;
        sat.prepare(kFs);
        sat.set_shape(SaturatorShape::cubic_soft);
        sat.set_bias(bias);
        sat.set_drive_db(24.0);
        sat.set_alias_policy(SaturatorAliasPolicy::adaa_iir);
        Saturator64 off;
        off.prepare(kFs);
        off.set_shape(SaturatorShape::cubic_soft);
        off.set_bias(bias);
        off.set_drive_db(24.0);
        off.set_alias_policy(SaturatorAliasPolicy::off);
        // A slow sine: aside from the kernel delay, both follow the same curve.
        double worst = 0.0;
        const double delay = sat.group_delay_samples();
        for (int n = 0; n < 2000; ++n) {
            const double x = 0.8 * std::sin(2.0 * kPi * 30.0 * n / kFs);
            const double y = sat.process(x);
            if (n < 50)
                continue;
            const double xd = 0.8 * std::sin(2.0 * kPi * 30.0 * (n - delay) / kFs);
            worst = std::max(worst, std::abs(y - off.shaped(xd)));
            REQUIRE(std::isfinite(y));
        }
        INFO("bias " << bias);
        CHECK(worst < 5e-3);
    }
}

TEST_CASE("Antiderivative IIR survives enormous steps", "[signal][adaa-iir][robustness]") {
    AntiderivativeIir iir;
    for (const double x : {1e30, -1e30, 0.5, 1e300, -1e300, 0.0}) {
        const double y = iir.process(TanhNonlinearity{}, x);
        const double z = iir.process(HardClipNonlinearity{}, -x);
        CHECK(std::isfinite(y));
        CHECK(std::isfinite(z));
    }
}

TEST_CASE("Antiderivative IIR is real-time safe and recovers from a non-finite input",
          "[signal][adaa-iir][rt-safety]") {
    AntiderivativeIir iir;
    Saturator64 sat;
    sat.prepare(kFs);
    sat.set_alias_policy(SaturatorAliasPolicy::adaa_iir);
    sat.set_drive_db(30.0);
    double sink = 0.0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int n = 0; n < 20000; ++n) {
            const double x = 12.0 * std::sin(0.31 * n) + ((n % 997) == 0 ? 40.0 : 0.0);
            sink += iir.process(TanhNonlinearity{}, x);
            sink += iir.process(HardClipNonlinearity{}, x);
            sink += sat.process(static_cast<double>(std::sin(0.45 * n)));
        }
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(std::isfinite(sink));
    CHECK(iir.process(TanhNonlinearity{}, std::nan("")) == 0.0);
    CHECK(std::isfinite(iir.process(TanhNonlinearity{}, 0.5)));
}

TEST_CASE("Antiderivative IIR kernel sweep report", "[.][adaa-iir][report]") {
    // Not run by default. Reruns the measurement behind the default kernel
    // choice: alias level for tanh at +30 dB drive on a 1 kHz sine at 44.1 kHz,
    // magnitude at a quarter of the rate, and low-frequency group delay.
    const auto x = sine(1.0);
    const double amplitude = std::pow(10.0, 30.0 / 20.0);
    std::vector<double> y(kN);
    for (int order = 2; order <= 8; ++order) {
        for (const double fc : {0.2, 0.25, 0.3, 0.35, 0.4, 0.45}) {
            const auto kernel = AntiderivativeIirKernel::butterworth(order, fc);
            AntiderivativeIir iir(kernel);
            for (int n = 0; n < kN; ++n)
                y[static_cast<std::size_t>(n)] =
                    iir.process(TanhNonlinearity{}, amplitude * x[static_cast<std::size_t>(n)]) /
                    amplitude;
            WARN("order " << order << " cutoff " << fc << " alias " << alias_db(y)
                          << " dB, at fs/4 " << 20.0 * std::log10(kernel.magnitude(0.25))
                          << " dB, delay " << kernel.group_delay_samples() << " samples");
        }
    }
    SUCCEED();
}
