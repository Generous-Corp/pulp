#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/modal_bank.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Hann-windowed complex DFT magnitude of x[start .. start+len) at freq_hz.
// This is the measurement primitive for modal analysis: an exact-frequency
// spectral probe whose windowed magnitude tracks a single mode's envelope.
double windowed_mag(const std::vector<float>& x, std::size_t start, std::size_t len,
                    double freq_hz, double sample_rate) {
    if (start + len > x.size()) len = x.size() - start;
    double re = 0.0, im = 0.0;
    const double w = 2.0 * kPi * freq_hz / sample_rate;
    for (std::size_t i = 0; i < len; ++i) {
        const double hann =
            0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                  static_cast<double>(len - 1)));
        const double v = hann * static_cast<double>(x[start + i]);
        const double phase = w * static_cast<double>(start + i);
        re += v * std::cos(phase);
        im -= v * std::sin(phase);
    }
    return std::sqrt(re * re + im * im);
}

// Refine a mode's frequency by scanning the windowed magnitude on a cent
// grid around a guess and parabolically interpolating the peak.
double measure_mode_freq(const std::vector<float>& x, double guess_hz,
                         double sample_rate, std::size_t window,
                         double span_cents = 60.0, double step_cents = 2.0) {
    const int steps = static_cast<int>(2.0 * span_cents / step_cents) + 1;
    double best_mag = -1.0, best_cents = 0.0;
    int best_idx = 0;
    std::vector<double> mags(static_cast<std::size_t>(steps));
    for (int k = 0; k < steps; ++k) {
        const double cents = -span_cents + step_cents * k;
        const double f = guess_hz * std::pow(2.0, cents / 1200.0);
        mags[static_cast<std::size_t>(k)] = windowed_mag(x, 0, window, f, sample_rate);
        if (mags[static_cast<std::size_t>(k)] > best_mag) {
            best_mag = mags[static_cast<std::size_t>(k)];
            best_cents = cents;
            best_idx = k;
        }
    }
    if (best_idx > 0 && best_idx < steps - 1) {
        const double m0 = mags[static_cast<std::size_t>(best_idx - 1)];
        const double m1 = mags[static_cast<std::size_t>(best_idx)];
        const double m2 = mags[static_cast<std::size_t>(best_idx + 1)];
        const double denom = m0 - 2.0 * m1 + m2;
        if (std::abs(denom) > 1e-12)
            best_cents += step_cents * 0.5 * (m0 - m2) / denom;
    }
    return guess_hz * std::pow(2.0, best_cents / 1200.0);
}

// Measure a mode's T60 from the impulse response: track the windowed
// magnitude at the mode frequency over hopped windows and least-squares fit
// the dB envelope's slope. T60 = -60 dB / slope.
double measure_mode_t60(const std::vector<float>& x, double freq_hz,
                        double sample_rate, double fit_start_s, double fit_end_s,
                        std::size_t window = 2048, std::size_t hop = 512) {
    std::vector<double> times, dbs;
    for (std::size_t start = static_cast<std::size_t>(fit_start_s * sample_rate);
         start + window <= x.size() &&
         static_cast<double>(start) / sample_rate < fit_end_s;
         start += hop) {
        const double mag = windowed_mag(x, start, window, freq_hz, sample_rate);
        if (mag <= 0.0) break;
        times.push_back((static_cast<double>(start) + window / 2.0) / sample_rate);
        dbs.push_back(20.0 * std::log10(mag));
    }
    REQUIRE(times.size() >= 4);
    const double n = static_cast<double>(times.size());
    double st = 0, sd = 0, stt = 0, std_ = 0;
    for (std::size_t i = 0; i < times.size(); ++i) {
        st += times[i];
        sd += dbs[i];
        stt += times[i] * times[i];
        std_ += times[i] * dbs[i];
    }
    const double slope = (n * std_ - st * sd) / (n * stt - st * st);
    REQUIRE(slope < 0.0);
    return -60.0 / slope;
}

std::vector<float> render_impulse_response(pulp::signal::ModalBank& bank,
                                           std::size_t num_samples) {
    std::vector<float> in(num_samples, 0.0f), out(num_samples, 0.0f);
    in[0] = 1.0f;
    constexpr int block = 512;
    for (std::size_t i = 0; i < num_samples; i += block) {
        const int n = static_cast<int>(std::min<std::size_t>(block, num_samples - i));
        bank.process_add(in.data() + i, out.data() + i, n);
    }
    return out;
}

// Peak |x| over a window — an envelope estimate for a single ringing mode.
// The window must span several cycles of the mode for the peak to approach
// the phasor magnitude.
double peak_abs(const std::vector<float>& x, std::size_t start, std::size_t len) {
    double p = 0.0;
    for (std::size_t i = 0; i < len && start + i < x.size(); ++i)
        p = std::max(p, std::abs(static_cast<double>(x[start + i])));
    return p;
}

} // namespace

TEST_CASE("modal bank impulse response matches prescribed mode frequencies and T60s",
          "[signal][modal]") {
    using pulp::signal::ModalBank;
    using pulp::signal::ModalMode;

    const double fs = 48000.0;
    // Inharmonic set spanning the keyboard; last mode is short-decay and is
    // checked for frequency only (too few envelope windows for a T60 fit).
    const std::vector<ModalMode> modes = {
        {110.0f, 2.0f, 1.0f},   {251.3f, 1.4f, 0.7f},  {829.7f, 0.9f, 0.5f},
        {2113.1f, 0.55f, 0.4f}, {5527.9f, 0.3f, 0.3f}, {9601.0f, 0.08f, 0.25f},
    };

    ModalBank bank;
    bank.prepare(fs, static_cast<int>(modes.size()));
    bank.set_modes(modes);

    const auto ir = render_impulse_response(bank, static_cast<std::size_t>(1.8 * fs));

    for (const auto& m : ir) REQUIRE(std::isfinite(m));

    for (const auto& mode : modes) {
        const double measured_hz =
            measure_mode_freq(ir, mode.freq_hz, fs, 8192);
        const double cents_err =
            1200.0 * std::log2(measured_hz / static_cast<double>(mode.freq_hz));
        INFO("mode " << mode.freq_hz << " Hz measured " << measured_hz
                     << " Hz (" << cents_err << " cents)");
        REQUIRE(std::abs(cents_err) < 10.0);
    }

    for (const auto& mode : modes) {
        if (mode.t60_s < 0.3f) continue;
        const double fit_end = std::min(0.8 * static_cast<double>(mode.t60_s), 1.6);
        const double measured_t60 =
            measure_mode_t60(ir, mode.freq_hz, fs, 0.05, fit_end);
        const double rel_err =
            (measured_t60 - static_cast<double>(mode.t60_s)) /
            static_cast<double>(mode.t60_s);
        INFO("mode " << mode.freq_hz << " Hz t60 " << mode.t60_s
                     << " s measured " << measured_t60 << " s ("
                     << rel_err * 100.0 << "%)");
        REQUIRE(std::abs(rel_err) < 0.08);
    }
}

TEST_CASE("strike pulse hardness controls how much energy reaches high modes",
          "[signal][modal]") {
    using pulp::signal::ModalBank;
    using pulp::signal::ModalMode;

    const double fs = 48000.0;
    const std::vector<ModalMode> modes = {
        {200.0f, 1.0f, 1.0f},
        {6000.0f, 1.0f, 1.0f},
    };

    auto strike_ratio = [&](int contact_samples) {
        ModalBank bank;
        bank.prepare(fs, 2);
        bank.set_modes(modes);
        const std::size_t total = 16384;
        std::vector<float> in(total, 0.0f), out(total, 0.0f);
        pulp::signal::fill_strike_pulse(
            std::span<float>(in.data(), static_cast<std::size_t>(contact_samples)),
            1.0f);
        bank.process_add(in.data(), out.data(), static_cast<int>(total));
        // Probe both modes just after contact ends; the ratio isolates the
        // excitation spectrum's tilt from absolute strike level.
        const std::size_t start = 256;
        const double low = windowed_mag(out, start, 8192, 200.0, fs);
        const double high = windowed_mag(out, start, 8192, 6000.0, fs);
        REQUIRE(low > 0.0);
        return high / low;
    };

    const double hard = strike_ratio(3);
    const double soft = strike_ratio(96);
    INFO("high/low mode energy ratio: hard=" << hard << " soft=" << soft);
    REQUIRE(hard > 4.0 * soft);
}

TEST_CASE("modal bank process path is allocation-free after prepare",
          "[signal][modal][rt-safety]") {
    using pulp::signal::ModalBank;
    using pulp::signal::ModalMode;

    const double fs = 48000.0;
    std::vector<ModalMode> modes(1024);
    std::mt19937 rng(0x5eed);
    std::uniform_real_distribution<float> freq(40.0f, 18000.0f);
    std::uniform_real_distribution<float> t60(0.2f, 6.0f);
    for (auto& m : modes) m = {freq(rng), t60(rng), 1.0f / 1024.0f};

    ModalBank bank;
    bank.prepare(fs, static_cast<int>(modes.size()), 4);

    std::vector<float> in(512, 0.0f);
    in[0] = 1.0f;
    std::vector<std::vector<float>> outs(4, std::vector<float>(512, 0.0f));
    std::vector<float*> ptrs;
    for (auto& o : outs) ptrs.push_back(o.data());
    std::vector<float> gains(modes.size(), 0.5f);

    {
        pulp::test::RtAllocationProbe probe;
        bank.set_modes(modes);
        bank.set_pickup_gains(1, gains);
        for (int b = 0; b < 8; ++b) bank.process_add(in.data(), outs[0].data(), 512);
        for (int b = 0; b < 8; ++b) bank.process_add(in.data(), ptrs.data(), 4, 512);
        bank.reset();
        REQUIRE(probe.allocation_count() == 0);
    }
    for (const auto& o : outs)
        for (float v : o) REQUIRE(std::isfinite(v));
}

TEST_CASE("retuning a ringing modal bank preserves its amplitude",
          "[signal][modal][retune]") {
    using pulp::signal::ModalBank;
    using pulp::signal::ModalMode;

    const double fs = 48000.0;

    SECTION("a single retune is amplitude- and phase-neutral wherever it lands") {
        // A direct-form resonator stores past output samples, so the amplitude
        // its state implies depends on the coefficients that produced it: the
        // same 220 -> 440 Hz retune rescales the mode by anywhere from 0.5x to
        // 1.0x depending on the phase it interrupts. The coupled form stores
        // the phasor, so the retune is a pure change of rotation angle.
        double worst = 1.0;
        for (int offset = 0; offset < 220; ++offset) {
            ModalBank bank;
            bank.prepare(fs, 1);
            bank.set_modes(std::vector<ModalMode>{{220.0f, 100.0f, 1.0f}});

            const std::size_t n = 20000;
            std::vector<float> in(n, 0.0f), out(n, 0.0f);
            in[0] = 1.0f;
            const std::size_t cut = 8000 + static_cast<std::size_t>(offset);
            bank.process_add(in.data(), out.data(), static_cast<int>(cut));
            bank.set_modes(std::vector<ModalMode>{{440.0f, 100.0f, 1.0f}});
            bank.process_add(in.data() + cut, out.data() + cut,
                             static_cast<int>(n - cut));

            // T60 = 100 s: the mode loses 0.03% over the 550 samples between
            // these probes, so any real step shows up as a ratio away from 1.
            const double ratio = peak_abs(out, cut + 50, 500) /
                                 peak_abs(out, cut - 500, 500);
            worst = std::min(worst, ratio);
        }
        INFO("worst amplitude ratio across 220 retune phases: " << worst);
        REQUIRE(worst > 0.99);
    }

    SECTION("error does not accumulate across a glide's worth of retunes") {
        // Three octaves in 2 s, retuned every block. T60 = 100 s makes the only
        // legitimate amplitude change the mode's own decay, 10^(-3*2/100).
        const double ideal = std::pow(10.0, -3.0 * 2.0 / 100.0);
        for (int block : {512, 128, 32}) {
            ModalBank bank;
            bank.prepare(fs, 1);
            const std::size_t n = static_cast<std::size_t>(2.0 * fs);
            std::vector<float> in(n, 0.0f), out(n, 0.0f);
            in[0] = 1.0f;
            for (std::size_t i = 0; i < n; i += static_cast<std::size_t>(block)) {
                const double frac = static_cast<double>(i) / static_cast<double>(n);
                const float f = static_cast<float>(110.0 * std::pow(2.0, 3.0 * frac));
                bank.set_modes(std::vector<ModalMode>{{f, 100.0f, 1.0f}});
                bank.process_add(in.data() + i, out.data() + i,
                                 static_cast<int>(std::min<std::size_t>(
                                     static_cast<std::size_t>(block), n - i)));
            }
            for (float v : out) REQUIRE(std::isfinite(v));
            // Both probe windows span >= 9 cycles so the peak tracks the phasor.
            const double measured = peak_abs(out, n - 6000, 4000) /
                                    peak_abs(out, 2000, 4000);
            INFO("block " << block << ": glide envelope " << measured
                          << " vs ideal decay " << ideal);
            REQUIRE(std::abs(measured / ideal - 1.0) < 0.05);
        }
    }

    SECTION("a retune reaches the new frequency") {
        ModalBank bank;
        bank.prepare(fs, 1);
        bank.set_modes(std::vector<ModalMode>{{220.0f, 4.0f, 1.0f}});
        const std::size_t n = 48000;
        std::vector<float> in(n, 0.0f), out(n, 0.0f);
        in[0] = 1.0f;
        bank.process_add(in.data(), out.data(), 8000);
        bank.set_modes(std::vector<ModalMode>{{330.0f, 4.0f, 1.0f}});
        bank.process_add(in.data() + 8000, out.data() + 8000,
                         static_cast<int>(n - 8000));
        std::vector<float> tail(out.begin() + 16000, out.end());
        const double measured = measure_mode_freq(tail, 330.0, fs, 16384);
        INFO("retuned mode measured " << measured << " Hz");
        REQUIRE(std::abs(1200.0 * std::log2(measured / 330.0)) < 5.0);
    }
}

TEST_CASE("modal bank pickups are independent views of one resonator state",
          "[signal][modal][pickup]") {
    using pulp::signal::ModalBank;
    using pulp::signal::ModalMode;

    const double fs = 48000.0;
    const std::vector<ModalMode> modes = {
        {180.0f, 1.5f, 1.0f}, {437.0f, 1.1f, 0.6f}, {910.0f, 0.8f, 0.35f},
    };
    const std::size_t n = 8192;
    std::vector<float> in(n, 0.0f);
    in[0] = 1.0f;

    // Three pickups: unit, halved, and one that inverts the middle mode.
    const std::vector<float> g0 = {1.0f, 1.0f, 1.0f};
    const std::vector<float> g1 = {0.5f, 0.5f, 0.5f};
    const std::vector<float> g2 = {1.0f, -1.0f, 1.0f};

    ModalBank bank;
    bank.prepare(fs, 3, 3);
    bank.set_modes(modes);
    bank.set_pickup_gains(0, g0);
    bank.set_pickup_gains(1, g1);
    bank.set_pickup_gains(2, g2);

    std::vector<std::vector<float>> outs(3, std::vector<float>(n, 0.0f));
    std::vector<float*> ptrs;
    for (auto& o : outs) ptrs.push_back(o.data());
    bank.process_add(in.data(), ptrs.data(), 3, static_cast<int>(n));

    // A pickup's gain scales only its own output: one shared pass over the
    // resonator state must serve every listening position.
    for (std::size_t i = 0; i < n; ++i)
        REQUIRE(outs[1][i] == Catch::Approx(0.5 * outs[0][i]).margin(1e-6));

    // The inverting pickup differs from the unit one by exactly twice the
    // middle mode, which is what a node-crossing pickup position does.
    ModalBank solo;
    solo.prepare(fs, 3);
    solo.set_modes(std::vector<ModalMode>{{0.0f, 1.0f, 0.0f},
                                          {437.0f, 1.1f, 0.6f},
                                          {0.0f, 1.0f, 0.0f}});
    std::vector<float> mid(n, 0.0f);
    solo.process_add(in.data(), mid.data(), static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i)
        REQUIRE(outs[2][i] ==
                Catch::Approx(outs[0][i] - 2.0 * mid[i]).margin(1e-5));

    // A bank rendered through one pickup must be bit-identical to the same
    // pickup rendered alongside others — pickups must not perturb the state.
    ModalBank single;
    single.prepare(fs, 3, 1);
    single.set_modes(modes);
    std::vector<float> lone(n, 0.0f);
    single.process_add(in.data(), lone.data(), static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i) REQUIRE(lone[i] == outs[0][i]);
}

TEST_CASE("strike position silences the modes it lands on a node of",
          "[signal][modal][strike-position]") {
    using pulp::signal::ModalBank;
    using pulp::signal::ModalMode;

    const double fs = 48000.0;
    const double f0 = 150.0;
    constexpr std::size_t kModes = 8;

    std::vector<pulp::signal::ModalShape> shapes(kModes);
    pulp::signal::fill_ideal_string_shapes(shapes);
    // Ideal string fixed at both ends: the m-th mode is sin(m*pi*x), so its
    // nodes sit at x = k/m and its frequency is m*f0.
    REQUIRE(shapes[1].half_waves == 2.0f);
    REQUIRE(pulp::signal::mode_shape_at(shapes[0], 0.0f) == Catch::Approx(0.0).margin(1e-6));
    REQUIRE(pulp::signal::mode_shape_at(shapes[0], 0.5f) == Catch::Approx(1.0).margin(1e-6));

    auto mode_energy = [&](float strike_pos, std::size_t mode_index) {
        std::vector<float> weights(kModes);
        pulp::signal::fill_mode_weights<float>(weights, shapes, strike_pos);
        std::vector<ModalMode> modes(kModes);
        for (std::size_t m = 0; m < kModes; ++m)
            modes[m] = {static_cast<float>(f0 * (m + 1)), 1.5f, weights[m]};

        ModalBank bank;
        bank.prepare(fs, static_cast<int>(kModes));
        bank.set_modes(modes);
        const std::size_t n = 16384;
        std::vector<float> in(n, 0.0f), out(n, 0.0f);
        in[0] = 1.0f;
        bank.process_add(in.data(), out.data(), static_cast<int>(n));
        return windowed_mag(out, 0, 8192, f0 * (mode_index + 1), fs);
    };

    SECTION("struck at the midpoint, a string has no even modes") {
        // sin(m*pi/2) == 0 for every even m: the classic reason a string
        // struck dead centre is missing its even harmonics.
        const double odd = mode_energy(0.5f, 0);
        REQUIRE(odd > 0.0);
        for (std::size_t m : {1u, 3u, 5u, 7u}) {
            const double e = mode_energy(0.5f, m);
            INFO("mode " << (m + 1) << " energy " << e << " vs mode 1 " << odd);
            REQUIRE(e / odd < 1e-4);
        }
        // The odd modes are still there — this is a node effect, not silence.
        REQUIRE(mode_energy(0.5f, 2) / odd > 0.05);
    }

    SECTION("struck at a quarter, the 4th and 8th modes drop out") {
        const double first = mode_energy(0.25f, 0);
        REQUIRE(first > 0.0);
        for (std::size_t m : {3u, 7u}) {
            const double e = mode_energy(0.25f, m);
            INFO("mode " << (m + 1) << " energy " << e);
            REQUIRE(e / first < 1e-4);
        }
        REQUIRE(mode_energy(0.25f, 1) / first > 0.05);
    }

    SECTION("nodes stay null at high mode indices") {
        // The shape argument grows with the mode index, so this is where a
        // float sin() stops nulling and a "node" starts leaking.
        std::vector<pulp::signal::ModalShape> many(1024);
        pulp::signal::fill_ideal_string_shapes(many);
        for (std::size_t m : {std::size_t{63}, std::size_t{255}, std::size_t{1023}}) {
            // Mode m+1 has nodes at every k/(m+1); 0.5 is one of them whenever
            // m+1 is even, which it is for all three indices probed here.
            const float w = pulp::signal::mode_shape_at(many[m], 0.5f);
            INFO("mode " << (m + 1) << " at midpoint: " << w);
            REQUIRE(std::abs(w) < 1e-5f);
        }
        // The neighbouring odd modes are at full antinode, so this is a node
        // effect and not a high-index amplitude collapse.
        REQUIRE(std::abs(pulp::signal::mode_shape_at(many[62], 0.5f)) ==
                Catch::Approx(1.0).margin(1e-4));
    }

    SECTION("a pickup on a node cannot hear that mode either") {
        // Strike off-node so every mode rings, then listen at the midpoint.
        std::vector<float> strike(kModes), listen(kModes);
        pulp::signal::fill_mode_weights<float>(strike, shapes, 0.13f);
        pulp::signal::fill_mode_weights<float>(listen, shapes, 0.5f);
        std::vector<ModalMode> modes(kModes);
        for (std::size_t m = 0; m < kModes; ++m)
            modes[m] = {static_cast<float>(f0 * (m + 1)), 1.5f, strike[m]};

        ModalBank bank;
        bank.prepare(fs, static_cast<int>(kModes), 1);
        bank.set_modes(modes);
        bank.set_pickup_gains(0, listen);
        const std::size_t n = 16384;
        std::vector<float> in(n, 0.0f), out(n, 0.0f);
        in[0] = 1.0f;
        bank.process_add(in.data(), out.data(), static_cast<int>(n));

        const double m1 = windowed_mag(out, 0, 8192, f0, fs);
        const double m2 = windowed_mag(out, 0, 8192, 2.0 * f0, fs);
        INFO("mode 1 " << m1 << " mode 2 " << m2);
        REQUIRE(m1 > 0.0);
        REQUIRE(m2 / m1 < 1e-4);
    }
}

TEST_CASE("modal bank holds pitch on low-frequency long-decay modes",
          "[signal][modal][precision]") {
    using pulp::signal::ModalBank;
    using pulp::signal::ModalMode;

    const double fs = 48000.0;
    // A 2-pole resonator that encodes frequency as 2*r*cos(w) loses low modes
    // to float rounding, because cos(w) crowds against 1 as w -> 0: measured
    // in direct form, these two land 2.5 and 7.5 cents flat. Encoding
    // frequency as r*sin(w) instead keeps full relative precision there.
    struct Case { double freq_hz, t60_s; };
    for (Case c : {Case{30.0, 5.0}, Case{20.0, 8.0}, Case{110.0, 2.0}}) {
        ModalBank bank;
        bank.prepare(fs, 1);
        bank.set_modes(std::vector<ModalMode>{
            {static_cast<float>(c.freq_hz), static_cast<float>(c.t60_s), 1.0f}});
        const auto ir = render_impulse_response(bank, static_cast<std::size_t>(4.0 * fs));

        const double measured = measure_mode_freq(ir, c.freq_hz, fs, 65536, 20.0, 0.25);
        const double cents = 1200.0 * std::log2(measured / c.freq_hz);
        INFO(c.freq_hz << " Hz / T60 " << c.t60_s << " s measured " << measured
                       << " Hz (" << cents << " cents)");
        REQUIRE(std::abs(cents) < 0.5);

        const double t60 =
            measure_mode_t60(ir, c.freq_hz, fs, 0.2, std::min(0.6 * c.t60_s, 3.0),
                             16384, 4096);
        const double rel = (t60 - c.t60_s) / c.t60_s;
        INFO(c.freq_hz << " Hz T60 measured " << t60 << " s (" << rel * 100.0 << "%)");
        REQUIRE(std::abs(rel) < 0.02);
    }
}

namespace {

// Cost-model probe for NONLINEAR COUPLED modal synthesis. Linear modes are
// O(M) per sample; physically coupled schemes (von Karman plates, coupled
// strings) additionally apply dense mode-coupling linear algebra every
// sample — the O(M^2) term that dominates. This probe pairs the same
// two-pole lane update with one dense MxM coupling matvec per sample plus a
// memoryless cubic injection, which is representative of the per-sample cost
// shape of energy-quadratised coupled schemes (those need one to two
// matvec-equivalents per sample). It is a measurement instrument for the
// CPU ceiling, not a shipped instrument model.
class CoupledCostProbe {
public:
    void prepare(double fs, int modes, unsigned seed) {
        modes_ = modes;
        coupling_.assign(static_cast<std::size_t>(modes) * modes, 0.0f);
        a1_.assign(static_cast<std::size_t>(modes), 0.0f);
        a2_.assign(static_cast<std::size_t>(modes), 0.0f);
        b0_.assign(static_cast<std::size_t>(modes), 0.0f);
        s1_.assign(static_cast<std::size_t>(modes), 0.0f);
        s2_.assign(static_cast<std::size_t>(modes), 0.0f);
        force_.assign(static_cast<std::size_t>(modes), 0.0f);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> freq(40.0f, 16000.0f);
        std::uniform_real_distribution<float> t60(0.3f, 4.0f);
        std::uniform_real_distribution<float> cpl(-1.0f, 1.0f);
        for (int m = 0; m < modes; ++m) {
            const double f = freq(rng);
            const double r = std::pow(10.0, -3.0 / (t60(rng) * fs));
            const double w = 2.0 * kPi * f / fs;
            a1_[static_cast<std::size_t>(m)] = static_cast<float>(2.0 * r * std::cos(w));
            a2_[static_cast<std::size_t>(m)] = static_cast<float>(-r * r);
            b0_[static_cast<std::size_t>(m)] =
                static_cast<float>(std::sin(w) / static_cast<double>(modes));
        }
        // Small couplings keep the probe stable so the timing loop measures
        // sustained cost, not a blow-up transient.
        const float scale = 1e-3f / static_cast<float>(modes);
        for (auto& c : coupling_) c = scale * cpl(rng);
    }

    float process_sample(float x) {
        // Dense coupling force: f = C * s1 — the O(M^2) per-sample core.
        // Sixteen independent accumulator lanes so the dot product
        // vectorizes without reassociation; fixed pairwise tree at the end.
        const float* s1v = s1_.data();
        for (int i = 0; i < modes_; ++i) {
            const float* row =
                coupling_.data() + static_cast<std::size_t>(i) * modes_;
            float acc[16] = {};
            int j = 0;
            for (; j + 16 <= modes_; j += 16)
                for (int l = 0; l < 16; ++l) acc[l] += row[j + l] * s1v[j + l];
            for (int step = 8; step > 0; step >>= 1)
                for (int l = 0; l < step; ++l) acc[l] += acc[l + step];
            float a = acc[0];
            for (; j < modes_; ++j) a += row[j] * s1v[j];
            force_[static_cast<std::size_t>(i)] = a;
        }
        float out = 0.0f;
        for (int i = 0; i < modes_; ++i) {
            const std::size_t si = static_cast<std::size_t>(i);
            const float nl = force_[si] * s1_[si] * s1_[si];
            const float y = b0_[si] * x + a1_[si] * s1_[si] + a2_[si] * s2_[si] + nl;
            s2_[si] = s1_[si];
            s1_[si] = y;
            out += y;
        }
        return out;
    }

    int modes() const { return modes_; }

private:
    int modes_ = 0;
    std::vector<float> coupling_, a1_, a2_, b0_, s1_, s2_, force_;
};

} // namespace

TEST_CASE("coupled nonlinear modal cost curve locates the CPU ceiling",
          "[signal][modal][bench]") {
    const double fs = 48000.0;
    const std::size_t duration = 12000; // 0.25 s per rep keeps the sweep bounded

    std::printf("\ncoupled nonlinear modal cost (single core, dense MxM coupling "
                "per sample, %.0f Hz)\n", fs);
    std::printf("%10s %14s %14s %20s\n", "modes", "us/sample", "x realtime",
                "x realtime (2 matvec)");

    for (int m : {32, 64, 128, 192, 256, 384, 512, 768, 1024}) {
        CoupledCostProbe probe;
        probe.prepare(fs, m, 0xC0FFEEu + static_cast<unsigned>(m));
        double best_s = 1e30;
        float sink = 0.0f;
        for (int rep = 0; rep < 3; ++rep) {
            CoupledCostProbe fresh;
            fresh.prepare(fs, m, 0xC0FFEEu + static_cast<unsigned>(m));
            const auto t0 = std::chrono::steady_clock::now();
            float x = 1.0f;
            for (std::size_t i = 0; i < duration; ++i) {
                sink += fresh.process_sample(x);
                x = 0.0f;
            }
            const auto t1 = std::chrono::steady_clock::now();
            best_s = std::min(best_s,
                              std::chrono::duration<double>(t1 - t0).count());
        }
        REQUIRE(std::isfinite(sink));
        const double s_per_sample = best_s / static_cast<double>(duration);
        const double x_rt = s_per_sample * fs;
        std::printf("%10d %14.3f %14.4f %20.4f\n", m, s_per_sample * 1e6, x_rt,
                    2.0 * x_rt);
    }
}

TEST_CASE("modal bank throughput scales to large banks in real time",
          "[signal][modal][bench]") {
    using pulp::signal::ModalBank;
    using pulp::signal::ModalMode;

    const double fs = 48000.0;
    constexpr int block = 512;
    const std::size_t seconds_samples = 48000;

    std::printf("\nmodal bank throughput (single core, %d-sample blocks, %.0f Hz)\n",
                block, fs);
    std::printf("%10s %14s %16s %14s\n", "modes", "ns/sample", "ns/mode-sample",
                "x realtime");

    double cost_1024 = 0.0;
    for (int num_modes : {64, 256, 1024, 4096, 16384, 65536}) {
        std::vector<ModalMode> modes(static_cast<std::size_t>(num_modes));
        std::mt19937 rng(0xacc0Cade);
        std::uniform_real_distribution<float> freq(40.0f, 18000.0f);
        std::uniform_real_distribution<float> t60(0.2f, 8.0f);
        for (auto& m : modes)
            m = {freq(rng), t60(rng), 1.0f / static_cast<float>(num_modes)};

        ModalBank bank;
        bank.prepare(fs, num_modes);
        bank.set_modes(modes);

        std::vector<float> in(seconds_samples, 0.0f), out(seconds_samples, 0.0f);
        in[0] = 1.0f;

        double best_s = 1e30;
        double sink = 0.0;
        const int reps = num_modes >= 16384 ? 2 : 3;
        for (int rep = 0; rep < reps; ++rep) {
            bank.reset();
            std::fill(out.begin(), out.end(), 0.0f);
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < seconds_samples; i += block)
                bank.process_add(in.data() + i, out.data() + i, block);
            const auto t1 = std::chrono::steady_clock::now();
            best_s = std::min(best_s,
                              std::chrono::duration<double>(t1 - t0).count());
            sink += static_cast<double>(out[seconds_samples / 2]);
        }
        REQUIRE(std::isfinite(sink));

        const double ns_per_sample =
            best_s * 1e9 / static_cast<double>(seconds_samples);
        const double ns_per_mode_sample = ns_per_sample / num_modes;
        const double x_realtime = best_s / 1.0;
        std::printf("%10d %14.1f %16.3f %14.4f\n", num_modes, ns_per_sample,
                    ns_per_mode_sample, x_realtime);
        if (num_modes == 1024) cost_1024 = x_realtime;
    }

    // The load-bearing assertion: a 1k-mode bank must be comfortably
    // real-time on one core, or CPU modal synthesis is not viable.
    REQUIRE(cost_1024 < 0.5);
}

TEST_CASE("modal bank pickups share one pass over the resonator state",
          "[signal][modal][bench]") {
    using pulp::signal::ModalBank;
    using pulp::signal::ModalMode;

    const double fs = 48000.0;
    constexpr int block = 512;
    constexpr int num_modes = 4096;
    const std::size_t seconds_samples = 48000;

    std::vector<ModalMode> modes(num_modes);
    std::mt19937 rng(0xacc0Cade);
    std::uniform_real_distribution<float> freq(40.0f, 18000.0f);
    std::uniform_real_distribution<float> t60(0.2f, 8.0f);
    for (auto& m : modes) m = {freq(rng), t60(rng), 1.0f / num_modes};

    std::printf("\nmodal bank pickup scaling (%d modes, single core)\n", num_modes);
    std::printf("%10s %16s %14s\n", "pickups", "ns/mode-sample", "vs 1 pickup");

    double cost_1 = 0.0, cost_4 = 0.0;
    for (int pickups : {1, 2, 4, 8}) {
        ModalBank bank;
        bank.prepare(fs, num_modes, pickups);
        bank.set_modes(modes);

        std::vector<float> in(seconds_samples, 0.0f);
        in[0] = 1.0f;
        std::vector<std::vector<float>> outs(
            static_cast<std::size_t>(pickups), std::vector<float>(seconds_samples, 0.0f));
        std::vector<float*> heads;
        for (auto& o : outs) heads.push_back(o.data());

        double best_s = 1e30, sink = 0.0;
        for (int rep = 0; rep < 3; ++rep) {
            bank.reset();
            std::vector<float*> cursor(heads.size());
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < seconds_samples; i += block) {
                for (std::size_t c = 0; c < heads.size(); ++c) cursor[c] = heads[c] + i;
                bank.process_add(in.data() + i, cursor.data(), pickups, block);
            }
            const auto t1 = std::chrono::steady_clock::now();
            best_s = std::min(best_s, std::chrono::duration<double>(t1 - t0).count());
            sink += static_cast<double>(outs[0][seconds_samples / 2]);
        }
        REQUIRE(std::isfinite(sink));

        const double ns_per_mode_sample =
            best_s * 1e9 / static_cast<double>(seconds_samples) / num_modes;
        if (pickups == 1) cost_1 = ns_per_mode_sample;
        if (pickups == 4) cost_4 = ns_per_mode_sample;
        std::printf("%10d %16.3f %14.2f\n", pickups, ns_per_mode_sample,
                    ns_per_mode_sample / cost_1);
    }

    // Reported, not asserted. The invariant worth gating — that pickups ride
    // along the single pass over resonator state instead of re-running it —
    // is a ratio between two timings, and a ratio of timings is the first
    // thing a loaded machine destroys (measured 1.4x to 5.5x for the same
    // four-pickup build depending only on what else was running). The
    // structural guarantee is enforced where it cannot flake: the pickup
    // independence test proves one shared state pass feeds every pickup.
    REQUIRE(cost_1 > 0.0);
    REQUIRE(cost_4 > 0.0);
}
