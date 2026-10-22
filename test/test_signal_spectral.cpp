#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/signal.hpp>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ── WindowFunction ───────────────────────────────────────────────────────────

TEST_CASE("WindowFunction Hann", "[signal][window]") {
    auto w = WindowFunction::generate(256, WindowFunction::Type::hann);
    REQUIRE(w.size() == 256);
    REQUIRE_THAT(w[0], WithinAbs(0.0, 0.001));
    REQUIRE_THAT(w[128], WithinAbs(1.0, 0.001)); // Peak at center
    REQUIRE_THAT(w[255], WithinAbs(0.0, 0.001));
}

TEST_CASE("WindowFunction Blackman", "[signal][window]") {
    auto w = WindowFunction::generate(512, WindowFunction::Type::blackman);
    REQUIRE(w.size() == 512);
    REQUIRE(w[256] > 0.9f); // Near peak
    REQUIRE(w[0] < 0.01f);  // Near zero at edges
}

TEST_CASE("WindowFunction apply", "[signal][window]") {
    auto w = WindowFunction::generate(4, WindowFunction::Type::rectangular);
    float buf[] = {1.0f, 2.0f, 3.0f, 4.0f};
    WindowFunction::apply(buf, w);
    // Rectangular window should not change values
    REQUIRE_THAT(buf[0], WithinAbs(1.0, 0.001));
    REQUIRE_THAT(buf[3], WithinAbs(4.0, 0.001));
}

TEST_CASE("WindowFunction covers hamming flat-top and kaiser branches", "[signal][window][issue-645]") {
    auto hamming = WindowFunction::generate(5, WindowFunction::Type::hamming);
    REQUIRE_THAT(hamming.front(), WithinAbs(0.08f, 1e-5f));
    REQUIRE_THAT(hamming.back(), WithinAbs(0.08f, 1e-5f));
    REQUIRE_THAT(hamming[2], WithinAbs(1.0f, 1e-5f));

    auto flat_top = WindowFunction::generate(9, WindowFunction::Type::flat_top);
    REQUIRE(flat_top[4] > 0.99f);
    REQUIRE(std::abs(flat_top.front()) < 0.001f);

    auto kaiser_default = WindowFunction::generate(5, WindowFunction::Type::kaiser);
    auto kaiser_custom = WindowFunction::generate(5, WindowFunction::Type::kaiser, 6.0f);
    REQUIRE_THAT(kaiser_default.front(), WithinAbs(kaiser_default.back(), 1e-6f));
    REQUIRE(kaiser_default[2] > kaiser_default.front());
    REQUIRE(kaiser_custom.front() < kaiser_default.front());

    float shaped[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    WindowFunction::apply(shaped, hamming);
    REQUIRE_THAT(shaped[0], WithinAbs(0.08f, 1e-5f));
    REQUIRE_THAT(shaped[2], WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("WindowFunction handles empty and singleton windows",
          "[signal][window][issue-645]") {
    for (auto type : {
             WindowFunction::Type::rectangular,
             WindowFunction::Type::hann,
             WindowFunction::Type::hamming,
             WindowFunction::Type::blackman,
             WindowFunction::Type::flat_top,
             WindowFunction::Type::kaiser,
         }) {
        REQUIRE(WindowFunction::generate(0, type).empty());
        REQUIRE(WindowFunction::generate(-4, type).empty());

        auto single = WindowFunction::generate(1, type, 8.0f);
        REQUIRE(single.size() == 1);
        REQUIRE_THAT(single[0], WithinAbs(1.0f, 1e-6f));

        float sample[] = {0.25f};
        WindowFunction::apply(sample, single);
        REQUIRE_THAT(sample[0], WithinAbs(0.25f, 1e-6f));
    }
}

// ── FFT ──────────────────────────────────────────────────────────────────────

TEST_CASE("FFT forward/inverse round-trip", "[signal][fft]") {
    Fft fft(256);

    // Create a simple signal
    std::vector<std::complex<float>> data(256);
    for (int i = 0; i < 256; ++i)
        data[i] = {std::sin(2.0f * 3.14159f * 10.0f * i / 256.0f), 0.0f};

    auto original = data;

    fft.forward(data.data());
    fft.inverse(data.data());

    // Should recover original signal
    for (int i = 0; i < 256; ++i) {
        REQUIRE(std::abs(data[i].real() - original[i].real()) < 0.001f);
    }
}

TEST_CASE("FFT detects single frequency", "[signal][fft]") {
    constexpr int N = 1024;
    Fft fft(N);

    // Generate 100Hz sine at 44100 sample rate
    std::vector<std::complex<float>> data(N);
    float freq = 100.0f;
    float sr = 44100.0f;
    for (int i = 0; i < N; ++i)
        data[i] = {std::sin(2.0f * 3.14159f * freq * i / sr), 0.0f};

    fft.forward(data.data());

    // Find the peak bin
    int peak_bin = 0;
    float peak_mag = 0;
    for (int i = 1; i < N / 2; ++i) {
        float mag = std::abs(data[i]);
        if (mag > peak_mag) { peak_mag = mag; peak_bin = i; }
    }

    // Expected bin: freq * N / sr = 100 * 1024 / 44100 ≈ 2.32
    float expected_bin = freq * N / sr;
    REQUIRE(std::abs(peak_bin - expected_bin) < 2.0f);
}

TEST_CASE("FFT magnitude_db", "[signal][fft]") {
    constexpr int N = 256;
    Fft fft(N);

    std::vector<std::complex<float>> freq(N);
    std::vector<float> mag_db(N / 2);

    // DC component = 1.0 → 0 dB
    freq[0] = {1.0f, 0.0f};
    fft.magnitude_db(freq.data(), mag_db.data(), N / 2);
    REQUIRE_THAT(mag_db[0], WithinAbs(0.0, 0.01));

    // 0.01 → -40 dB
    freq[0] = {0.01f, 0.0f};
    fft.magnitude_db(freq.data(), mag_db.data(), 1);
    REQUIRE_THAT(mag_db[0], WithinAbs(-40.0, 0.1));
}

TEST_CASE("FFT forward_real", "[signal][fft]") {
    constexpr int N = 256;
    Fft fft(N);

    std::vector<float> input(N, 0.0f);
    input[0] = 1.0f; // Impulse

    std::vector<std::complex<float>> output(N);
    fft.forward_real(input.data(), output.data());

    // FFT of impulse: all bins should have magnitude 1
    for (int i = 0; i < N; ++i) {
        REQUIRE(std::abs(std::abs(output[i]) - 1.0f) < 0.01f);
    }
}

TEST_CASE("FFT move preserves transform state", "[signal][fft][issue-645]") {
    Fft original(8);
    Fft moved(std::move(original));

    REQUIRE(original.size() == 0);
    REQUIRE(moved.size() == 8);

    std::vector<std::complex<float>> data(8, {1.0f, 0.0f});
    moved.forward(data.data());

    REQUIRE_THAT(data[0].real(), WithinAbs(8.0f, 1e-4f));
    REQUIRE_THAT(data[0].imag(), WithinAbs(0.0f, 1e-4f));
    for (int i = 1; i < 8; ++i) {
        REQUIRE(std::abs(data[i]) < 1e-4f);
    }

    Fft assigned;
    assigned = std::move(moved);
    REQUIRE(moved.size() == 0);
    REQUIRE(assigned.size() == 8);

    assigned.inverse(data.data());
    for (const auto& sample : data) {
        REQUIRE_THAT(sample.real(), WithinAbs(1.0f, 1e-4f));
        REQUIRE_THAT(sample.imag(), WithinAbs(0.0f, 1e-4f));
    }
}

TEST_CASE("FFT move assignment replaces an existing transform",
          "[signal][fft]") {
    Fft source(8);
    Fft destination(16);

    destination = std::move(source);
    REQUIRE(source.size() == 0);
    REQUIRE(destination.size() == 8);

    auto& alias = destination;
    destination = std::move(alias);
    REQUIRE(destination.size() == 8);

    std::vector<std::complex<float>> data(8, {1.0f, 0.0f});
    destination.forward(data.data());
    REQUIRE_THAT(data[0].real(), WithinAbs(8.0f, 1e-4f));
}

TEST_CASE("FFT magnitude helpers handle silence and complex bins", "[signal][fft][issue-645]") {
    Fft fft(8);
    std::vector<std::complex<float>> freq = {
        {0.0f, 0.0f},
        {3.0f, 4.0f},
        {-0.5f, 0.0f},
        {0.0f, -2.0f},
        {0.0f, 0.0f},
        {0.0f, 0.0f},
        {0.0f, 0.0f},
        {0.0f, 0.0f},
    };
    std::vector<float> linear(4, 0.0f);
    std::vector<float> db(4, 0.0f);

    fft.magnitude(freq.data(), linear.data(), 4);
    fft.magnitude_db(freq.data(), db.data(), 4);

    REQUIRE_THAT(linear[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(linear[1], WithinAbs(5.0f, 1e-6f));
    REQUIRE_THAT(linear[2], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(linear[3], WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(db[0], WithinAbs(-200.0f, 0.01f));
    REQUIRE_THAT(db[1], WithinAbs(13.9794f, 0.01f));
    REQUIRE_THAT(db[2], WithinAbs(-6.0206f, 0.01f));
    REQUIRE_THAT(db[3], WithinAbs(6.0206f, 0.01f));
}

TEST_CASE("FFT forward_real preserves exact-bin conjugate symmetry", "[signal][fft][issue-645]") {
    constexpr int N = 64;
    constexpr int bin = 5;
    Fft fft(N);
    std::vector<float> input(N);
    for (int i = 0; i < N; ++i) {
        input[i] = std::sin(2.0f * 3.14159265358979323846f * bin * i / N);
    }

    std::vector<std::complex<float>> output(N);
    std::vector<float> magnitude(N, 0.0f);
    fft.forward_real(input.data(), output.data());
    fft.magnitude(output.data(), magnitude.data(), N);

    REQUIRE_THAT(magnitude[0], WithinAbs(0.0f, 1e-4f));
    REQUIRE_THAT(magnitude[bin], WithinAbs(static_cast<float>(N) / 2.0f, 1e-3f));
    REQUIRE_THAT(magnitude[N - bin], WithinAbs(static_cast<float>(N) / 2.0f, 1e-3f));
    REQUIRE_THAT(output[bin].real(), WithinAbs(output[N - bin].real(), 1e-4f));
    REQUIRE_THAT(output[bin].imag(), WithinAbs(-output[N - bin].imag(), 1e-4f));
}

TEST_CASE("FFT real transform exposes DC and Nyquist bins explicitly",
          "[signal][fft]") {
    constexpr int N = 16;
    Fft fft(N);
    std::vector<float> constant(N, 0.25f);
    std::vector<std::complex<float>> output(N);

    fft.forward_real(constant.data(), output.data());

    REQUIRE(fft.size() == N);
    REQUIRE_THAT(output[0].real(), WithinAbs(4.0f, 1e-5f));
    REQUIRE_THAT(output[0].imag(), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(output[N / 2].real(), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(output[N / 2].imag(), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(std::abs(output[0]), WithinAbs(4.0f, 1e-5f));
    REQUIRE_THAT(std::abs(output[N / 2]), WithinAbs(0.0f, 1e-5f));
    for (int i = 1; i < N / 2; ++i) {
        REQUIRE_THAT(output[i].real(), WithinAbs(output[N - i].real(), 1e-5f));
        REQUIRE_THAT(output[i].imag(), WithinAbs(-output[N - i].imag(), 1e-5f));
        REQUIRE(std::abs(output[i]) < 1e-5f);
    }
}

TEST_CASE("FFT magnitude helpers tolerate empty ranges and preserve destination values",
          "[signal][fft]") {
    Fft fft(8);
    std::vector<std::complex<float>> bins(8, {1.0f, 0.0f});
    std::vector<float> linear = {7.0f, 8.0f};
    std::vector<float> db = {9.0f, 10.0f};

    REQUIRE(fft.size() == 8);
    fft.magnitude(bins.data(), linear.data(), 0);
    fft.magnitude_db(bins.data(), db.data(), 0);

    REQUIRE_THAT(linear[0], WithinAbs(7.0f, 1e-6f));
    REQUIRE_THAT(linear[1], WithinAbs(8.0f, 1e-6f));
    REQUIRE_THAT(db[0], WithinAbs(9.0f, 1e-6f));
    REQUIRE_THAT(db[1], WithinAbs(10.0f, 1e-6f));
}

TEST_CASE("FFT64 runs a double-precision round trip and real transform",
          "[signal][fft][f64]") {
    constexpr int N = 16;
    Fft64 fft(N);

    std::vector<std::complex<double>> data(N);
    for (int i = 0; i < N; ++i) {
        data[static_cast<size_t>(i)] = {
            std::sin(2.0 * 3.14159265358979323846 * 3.0 * i / N), 0.0};
    }
    const auto original = data;

    fft.forward(data.data());
    fft.inverse(data.data());

    for (int i = 0; i < N; ++i) {
        REQUIRE_THAT(data[static_cast<size_t>(i)].real(),
                     WithinAbs(original[static_cast<size_t>(i)].real(), 1e-12));
        REQUIRE_THAT(data[static_cast<size_t>(i)].imag(), WithinAbs(0.0, 1e-12));
    }

    std::vector<double> real_input(N, 0.0);
    real_input[0] = 1.0;
    std::vector<std::complex<double>> bins(N);
    std::vector<double> magnitude(N, 0.0);
    fft.forward_real(real_input.data(), bins.data());
    fft.magnitude(bins.data(), magnitude.data(), N);

    for (double mag : magnitude)
        REQUIRE_THAT(mag, WithinAbs(1.0, 1e-12));

    bins[0] = {0.01, 0.0};
    std::vector<double> db(1, 0.0);
    fft.magnitude_db(bins.data(), db.data(), 1);
    REQUIRE_THAT(db[0], WithinAbs(-40.0, 1e-12));
}

// ── Convolver ────────────────────────────────────────────────────────────────

// ── Real-input transform ────────────────────────────────────────────────────

namespace {

// Deterministic broadband test signal: a few incommensurate tones plus a
// linear-congruential noise floor, so every bin carries energy.
template <typename T> std::vector<T> real_fft_stimulus(int n) {
    std::vector<T> x(static_cast<std::size_t>(n));
    std::uint32_t state = 0x9E3779B9u;
    for (int i = 0; i < n; ++i) {
        state = state * 1664525u + 1013904223u;
        const double noise = static_cast<double>(state >> 8) / 16777216.0 - 0.5;
        x[static_cast<std::size_t>(i)] =
            static_cast<T>(0.6 * std::sin(0.31 * i) + 0.25 * std::cos(2.9 * i + 0.4) + 0.1 * noise);
    }
    return x;
}

// Direct O(N^2) DFT in long double: the reference every transform is graded
// against.
std::vector<std::complex<long double>> reference_dft(const std::vector<double>& x) {
    const auto n = static_cast<long double>(x.size());
    std::vector<std::complex<long double>> out(x.size() / 2 + 1);
    const long double two_pi = 6.283185307179586476925286766559L;
    for (std::size_t k = 0; k < out.size(); ++k) {
        long double re = 0, im = 0;
        for (std::size_t t = 0; t < x.size(); ++t) {
            const long double phase = -two_pi * static_cast<long double>((k * t) % x.size()) / n;
            re += x[t] * std::cos(phase);
            im += x[t] * std::sin(phase);
        }
        out[k] = {re, im};
    }
    return out;
}

// Error energy over reference energy, in dB.
template <typename C>
double spectrum_error_db(const C* actual, const std::vector<std::complex<long double>>& reference,
                         double scale = 1.0) {
    long double err = 0, ref = 0;
    for (std::size_t k = 0; k < reference.size(); ++k) {
        const auto r = reference[k] * static_cast<long double>(scale);
        const std::complex<long double> a(actual[k].real(), actual[k].imag());
        err += std::norm(a - r);
        ref += std::norm(r);
    }
    return 10.0 * std::log10(static_cast<double>(err / ref) + 1e-300);
}

template <typename T> double signal_error_db(const T* actual, const std::vector<T>& expected) {
    double err = 0, ref = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const double d = static_cast<double>(actual[i]) - static_cast<double>(expected[i]);
        err += d * d;
        ref += static_cast<double>(expected[i]) * static_cast<double>(expected[i]);
    }
    return 10.0 * std::log10(err / ref + 1e-300);
}

} // namespace

TEST_CASE("FFT forward_real_half matches a long-double DFT and writes only N/2+1 bins",
          "[signal][fft][real]") {
    for (const int n : {2, 4, 8, 16, 64, 512, 2048, 4096}) {
        const auto x32 = real_fft_stimulus<float>(n);
        std::vector<double> x64(x32.begin(), x32.end());
        const auto reference = reference_dft(x64);

        Fft fft(n);
        REQUIRE(fft.real_bins() == n / 2 + 1);
        const float sentinel = std::numeric_limits<float>::quiet_NaN();
        std::vector<std::complex<float>> out(static_cast<std::size_t>(n) + 4, {sentinel, sentinel});
        fft.forward_real_half(x32.data(), out.data());
        const double err32 = spectrum_error_db(out.data(), reference);
        INFO("n=" << n << " float error=" << err32 << " dB");
        REQUIRE(err32 < -120.0);
        REQUIRE(out[0].imag() == 0.0f);
        REQUIRE(out[static_cast<std::size_t>(n / 2)].imag() == 0.0f);
        for (std::size_t k = static_cast<std::size_t>(n / 2) + 1; k < out.size(); ++k)
            REQUIRE(std::isnan(out[k].real()));

        // Scale folds into the pack: a quarter of the transform, not a rerun.
        std::vector<std::complex<float>> scaled(static_cast<std::size_t>(n / 2 + 1));
        fft.forward_real_half(x32.data(), scaled.data(), 0.25f);
        REQUIRE(spectrum_error_db(scaled.data(), reference, 0.25) < -120.0);

        Fft64 fft64(n);
        std::vector<std::complex<double>> out64(static_cast<std::size_t>(n / 2 + 1));
        fft64.forward_real_half(x64.data(), out64.data());
        const double err64 = spectrum_error_db(out64.data(), reference);
        INFO("double error=" << err64 << " dB");
        REQUIRE(err64 < -280.0);
    }
}

TEST_CASE("FFT real round trip recovers the signal, in place or not", "[signal][fft][real]") {
    for (const int n : {2, 4, 8, 32, 512, 4096}) {
        const auto x32 = real_fft_stimulus<float>(n);
        Fft fft(n);
        std::vector<std::complex<float>> bins(static_cast<std::size_t>(n / 2 + 1));
        fft.forward_real_half(x32.data(), bins.data());
        std::vector<float> back(static_cast<std::size_t>(n));
        fft.inverse_real_half(bins.data(), back.data());
        INFO("n=" << n);
        REQUIRE(signal_error_db(back.data(), x32) < -130.0);

        // Same spectrum transformed in its own storage: the complex buffer
        // needs N + 2 scalars, enough to hold the N real outputs.
        auto inplace = bins;
        fft.inverse_real_half(inplace.data(), reinterpret_cast<float*>(inplace.data()));
        const auto* inplace_samples = reinterpret_cast<const float*>(inplace.data());
        for (int i = 0; i < n; ++i)
            REQUIRE(inplace_samples[i] == back[static_cast<std::size_t>(i)]);

        // A real signal cannot carry imaginary DC or Nyquist; junk there is
        // ignored rather than leaking into the samples.
        auto junk = bins;
        junk[0].imag(123.0f);
        junk[static_cast<std::size_t>(n / 2)].imag(-77.0f);
        std::vector<float> ignored(static_cast<std::size_t>(n));
        fft.inverse_real_half(junk.data(), ignored.data());
        REQUIRE(ignored == back);

        const auto x64 = real_fft_stimulus<double>(n);
        Fft64 fft64(n);
        std::vector<std::complex<double>> bins64(static_cast<std::size_t>(n / 2 + 1));
        fft64.forward_real_half(x64.data(), bins64.data());
        std::vector<double> back64(static_cast<std::size_t>(n));
        fft64.inverse_real_half(bins64.data(), back64.data());
        REQUIRE(signal_error_db(back64.data(), x64) < -280.0);
        auto inplace64 = bins64;
        fft64.inverse_real_half(inplace64.data(), reinterpret_cast<double*>(inplace64.data()));
        REQUIRE(signal_error_db(reinterpret_cast<const double*>(inplace64.data()), x64) < -280.0);
    }
}

TEST_CASE("FFT real transform error gate sees a planted defect", "[signal][fft][real]") {
    // Negative control for the -120 dB gate: the same measurement against a
    // spectrum with one bin nudged by 1e-5 of the spectrum's RMS norm (a
    // -100 dB defect) must fail the gate, or the gate is checking nothing.
    constexpr int n = 512;
    const auto x32 = real_fft_stimulus<float>(n);
    const auto reference = reference_dft(std::vector<double>(x32.begin(), x32.end()));
    Fft fft(n);
    std::vector<std::complex<float>> out(n / 2 + 1);
    fft.forward_real_half(x32.data(), out.data());
    REQUIRE(spectrum_error_db(out.data(), reference) < -120.0);
    double energy = 0.0;
    for (const auto& bin : out)
        energy += std::norm(bin);
    out[37] += std::complex<float>(static_cast<float>(1e-5 * std::sqrt(energy)), 0.0f);
    REQUIRE(spectrum_error_db(out.data(), reference) > -120.0);
}

TEST_CASE("FFT forward_real equals the half spectrum mirrored", "[signal][fft][real]") {
    for (const int n : {8, 512, 2048}) {
        const auto x = real_fft_stimulus<float>(n);
        Fft fft(n);
        std::vector<std::complex<float>> full(static_cast<std::size_t>(n));
        std::vector<std::complex<float>> half(static_cast<std::size_t>(n / 2 + 1));
        fft.forward_real(x.data(), full.data());
        fft.forward_real_half(x.data(), half.data());
        for (int k = 0; k <= n / 2; ++k)
            REQUIRE(full[static_cast<std::size_t>(k)] == half[static_cast<std::size_t>(k)]);
        for (int k = 1; k < n / 2; ++k)
            REQUIRE(full[static_cast<std::size_t>(n - k)] ==
                    std::conj(half[static_cast<std::size_t>(k)]));
    }
}

TEST_CASE("Convolver with identity IR", "[signal][convolver]") {
    Convolver conv;

    // Identity IR: [1, 0, 0, ...]
    float ir[] = {1.0f};
    conv.load_ir(ir, 1, 64);

    // Process a known signal
    std::vector<float> input(256);
    std::vector<float> output(256);
    for (int i = 0; i < 256; ++i)
        input[i] = std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);

    conv.process(input.data(), output.data(), 256);

    // Output should match input (after initial latency of one block)
    float max_error = 0;
    for (int i = 64; i < 200; ++i) {
        float error = std::abs(output[i] - input[i - 64]);
        if (error > max_error) max_error = error;
    }
    REQUIRE(max_error < 0.01f);
}

TEST_CASE("Convolver with simple delay IR", "[signal][convolver]") {
    Convolver conv;

    // Delay IR: [0, 0, 0, 0, 1] — 4-sample delay
    float ir[] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    conv.load_ir(ir, 5, 32);

    // Send an impulse and collect output
    std::vector<float> output(128, 0.0f);
    output[0] = conv.process(1.0f);
    for (int i = 1; i < 128; ++i)
        output[i] = conv.process(0.0f);

    // Should see the impulse delayed by 4 samples + block latency
    float peak = 0;
    int peak_pos = 0;
    for (int i = 0; i < 128; ++i) {
        if (std::abs(output[i]) > peak) {
            peak = std::abs(output[i]);
            peak_pos = i;
        }
    }
    REQUIRE(peak > 0.9f); // Should find the delayed impulse
    REQUIRE(peak_pos >= 4); // At least 4 samples delayed
}

TEST_CASE("Convolver reset clears buffered overlap", "[signal][convolver][issue-645]") {
    Convolver conv;
    float ir[] = {0.0f, 1.0f, 0.5f};
    conv.load_ir(ir, 3, 8);

    std::vector<float> impulse(24, 0.0f);
    std::vector<float> output(24, 0.0f);
    impulse[0] = 1.0f;
    conv.process(impulse.data(), output.data(), static_cast<int>(output.size()));

    bool produced_tail = false;
    for (float sample : output) {
        produced_tail = produced_tail || std::abs(sample) > 1e-4f;
    }
    REQUIRE(produced_tail);

    conv.reset();
    std::fill(output.begin(), output.end(), 99.0f);
    std::fill(impulse.begin(), impulse.end(), 0.0f);
    conv.process(impulse.data(), output.data(), static_cast<int>(output.size()));

    for (float sample : output) {
        REQUIRE_THAT(sample, WithinAbs(0.0f, 1e-4f));
    }
}

TEST_CASE("Convolver unloaded and empty IR paths pass through safely",
          "[signal][convolver]") {
    Convolver conv;
    REQUIRE_THAT(conv.process(0.375f), WithinAbs(0.375f, 1e-6f));
    REQUIRE_THAT(conv.process(-0.25f), WithinAbs(-0.25f, 1e-6f));

    std::vector<float> input = {0.0f, 0.25f, -0.5f, 0.75f};
    std::vector<float> output(input.size(), 99.0f);
    conv.process(input.data(), output.data(), static_cast<int>(input.size()));
    REQUIRE_THAT(output[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(output[1], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(output[2], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(output[3], WithinAbs(0.75f, 1e-6f));
    for (size_t i = 0; i < input.size(); ++i)
        REQUIRE_THAT(output[i], WithinAbs(input[i], 1e-6f));

    float dummy = 1.0f;
    conv.load_ir(&dummy, 0, 8);
    REQUIRE_THAT(conv.process(0.5f), WithinAbs(0.5f, 1e-6f));

    conv.process(input.data(), output.data(), static_cast<int>(input.size()));
    REQUIRE_THAT(output[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(output[1], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(output[2], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(output[3], WithinAbs(0.75f, 1e-6f));
    for (size_t i = 0; i < input.size(); ++i)
        REQUIRE_THAT(output[i], WithinAbs(input[i], 1e-6f));

    conv.load_ir(nullptr, 4, 8);
    REQUIRE_THAT(conv.process(-0.75f), WithinAbs(-0.75f, 1e-6f));
}

TEST_CASE("Convolver default block size and reload reset the buffered stream",
          "[signal][convolver]") {
    Convolver conv;
    float identity[] = {1.0f};
    conv.load_ir(identity, 1);

    std::vector<float> warmup(256, 0.0f);
    std::vector<float> output(256, 99.0f);
    warmup[0] = 1.0f;
    conv.process(warmup.data(), output.data(), static_cast<int>(warmup.size()));

    REQUIRE_THAT(output[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(output[255], WithinAbs(0.0f, 1e-5f));

    std::vector<float> second(256, 0.0f);
    second[0] = 0.5f;
    conv.process(second.data(), output.data(), static_cast<int>(second.size()));

    REQUIRE_THAT(output[0], WithinAbs(1.0f, 1e-4f));
    REQUIRE_THAT(output[1], WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(output[2], WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(output[255], WithinAbs(0.0f, 1e-5f));

    conv.load_ir(identity, 1, 8);
    for (int i = 0; i < 7; ++i)
        REQUIRE_THAT(conv.process(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(conv.process(1.0f), WithinAbs(0.0f, 1e-6f));
    for (int i = 0; i < 7; ++i)
        REQUIRE_THAT(conv.process(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(conv.process(0.0f), WithinAbs(1.0f, 1e-4f));
}

TEST_CASE("Convolver64 identity IR preserves double buffers",
          "[signal][convolver][f64]") {
    Convolver64 conv;
    const double identity[] = {1.0};
    conv.load_ir(identity, 1, 8);

    std::vector<double> input(16, 0.0);
    std::vector<double> output(16, 0.0);
    input[0] = 0.25;
    input[1] = -0.5;
    conv.process(input.data(), output.data(), static_cast<int>(input.size()));

    REQUIRE_THAT(output[8], WithinAbs(0.25, 1e-12));
    REQUIRE_THAT(output[9], WithinAbs(-0.5, 1e-12));
}

TEST_CASE("Convolver overlap-add keeps the whole tail of an IR longer than one block",
          "[signal][convolver][null][regression]") {
    // Regression: an IR longer than the block leaves a tail reaching several
    // blocks ahead, and every block's tail must accumulate into the pending
    // overlap. Replacing the overlap with only the newest block's tail dropped
    // the older tails, so any such IR convolved wrongly (about -0.2 to -37 dB
    // null instead of -130). Every case here has ir.size() > block. Output is
    // delayed by exactly one block.
    std::vector<float> input(4096), ir(300);
    std::uint32_t state = 0x777u;
    for (auto& v : input) {
        state = state * 1664525u + 1013904223u;
        v = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
    }
    for (std::size_t i = 0; i < ir.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        ir[i] = (static_cast<float>(state >> 8) / 8388608.0f - 1.0f) *
                std::exp(-0.01f * static_cast<float>(i));
    }
    for (const int block : {1, 64, 256}) {
        REQUIRE(ir.size() > static_cast<std::size_t>(block));
        Convolver conv;
        conv.load_ir(ir.data(), static_cast<int>(ir.size()), block);
        std::vector<float> out(input.size());
        conv.process(input.data(), out.data(), static_cast<int>(input.size()));
        long double err = 0, ref = 0;
        for (std::size_t n = 0; n + static_cast<std::size_t>(block) < input.size(); ++n) {
            long double y = 0;
            for (std::size_t k = 0; k < ir.size() && k <= n; ++k)
                y += static_cast<long double>(ir[k]) * input[n - k];
            const long double d = out[n + static_cast<std::size_t>(block)] - y;
            err += d * d;
            ref += y * y;
        }
        const double null_db = 10.0 * std::log10(static_cast<double>(err / ref));
        INFO("block=" << block << " null=" << null_db << " dB");
        REQUIRE(null_db < -120.0);
    }
}

TEST_CASE("FFT transforms stay exact across every split placement", "[signal][fft][real]") {
    // The float transform places its vDSP split buffers on or off a 64-byte
    // boundary depending on the size and the transform; both placements, and
    // the switch-over sizes either side of each threshold, must round-trip.
    for (const int n : {256, 512, 2048, 4096}) {
        const auto x = real_fft_stimulus<float>(n);
        Fft fft(n);
        REQUIRE(fft.ready());
        std::vector<std::complex<float>> bins(static_cast<std::size_t>(n / 2 + 1));
        fft.forward_real_half(x.data(), bins.data());
        std::vector<float> back(static_cast<std::size_t>(n));
        fft.inverse_real_half(bins.data(), back.data());
        INFO("n=" << n);
        REQUIRE(signal_error_db(back.data(), x) < -130.0);

        std::vector<std::complex<float>> c(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            c[static_cast<std::size_t>(i)] = {x[static_cast<std::size_t>(i)],
                                              x[static_cast<std::size_t>(n - 1 - i)]};
        const auto original = c;
        fft.forward(c.data());
        fft.inverse(c.data());
        double err = 0.0, ref = 0.0;
        for (std::size_t i = 0; i < c.size(); ++i) {
            err += std::norm(c[i] - original[i]);
            ref += std::norm(original[i]);
        }
        REQUIRE(10.0 * std::log10(err / ref) < -130.0);

        // A moved transform keeps its placement valid.
        Fft moved(std::move(fft));
        REQUIRE(moved.ready());
        std::vector<float> again(static_cast<std::size_t>(n));
        moved.forward_real_half(x.data(), bins.data());
        moved.inverse_real_half(bins.data(), again.data());
        REQUIRE(again == back);
    }
}

TEST_CASE("Convolver overlap-add matches a direct convolution when the IR fits one block",
          "[signal][convolver][null]") {
    // The single-block case never needed the carried tail; it must keep nulling
    // after the tail fix and the move to real transforms.
    std::vector<float> input(2048), ir(50);
    std::uint32_t state = 0x31u;
    for (auto& v : input) {
        state = state * 1664525u + 1013904223u;
        v = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
    }
    for (auto& v : ir) {
        state = state * 1664525u + 1013904223u;
        v = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
    }
    for (const int block : {64, 512}) {
        Convolver conv;
        conv.load_ir(ir.data(), static_cast<int>(ir.size()), block);
        std::vector<float> out(input.size());
        conv.process(input.data(), out.data(), static_cast<int>(input.size()));
        long double err = 0, ref = 0;
        for (std::size_t n = 0; n + static_cast<std::size_t>(block) < input.size(); ++n) {
            long double y = 0;
            for (std::size_t k = 0; k < ir.size() && k <= n; ++k)
                y += static_cast<long double>(ir[k]) * input[n - k];
            const long double d = out[n + static_cast<std::size_t>(block)] - y;
            err += d * d;
            ref += y * y;
        }
        const double null_db = 10.0 * std::log10(static_cast<double>(err / ref));
        INFO("block=" << block << " null=" << null_db << " dB");
        REQUIRE(null_db < -120.0);
    }
}

TEST_CASE("FFT vDSP split halves sit 16 bytes past a 64-byte boundary",
          "[signal][fft][real][placement]") {
    // vDSP's 64-byte-aligned split path rounds 2.6-6.6 dB worse from 512
    // points up, so the transform keeps both halves off that boundary at every
    // size, including either side of every size that once switched paths.
    for (const int n : {32, 64, 512, 1024, 2048, 4096, 8192}) {
        Fft fft(n);
        INFO("n=" << n);
#if PULP_FFT_HAS_VDSP
        REQUIRE(fft.vdsp_split_offset_bytes(true) == 16);
        REQUIRE(fft.vdsp_split_offset_bytes(false) == 16);
        Fft moved(std::move(fft));
        REQUIRE(moved.vdsp_split_offset_bytes(true) == 16);
        REQUIRE(moved.vdsp_split_offset_bytes(false) == 16);
#else
        REQUIRE(fft.vdsp_split_offset_bytes(true) == -1);
#endif
    }
    REQUIRE(Fft64(512).vdsp_split_offset_bytes(true) == -1);
}

TEST_CASE("FFT real transform error matches the most accurate vDSP placement",
          "[signal][fft][real][accuracy]") {
    // Worst error over bins and frames relative to the largest bin, against a
    // long-double DFT: the metric the cross-framework quality runs use. Off the
    // 64-byte boundary vDSP measures -135.2 to -136.4 dB here; on it, -129.9 to
    // -132.7 dB. The gate sits between, so a placement that slides back onto
    // the boundary fails it.
    for (const int n : {512, 1024, 2048, 4096}) {
        Fft fft(n);
        double worst = 0.0;
        std::uint32_t state = 12345u + static_cast<std::uint32_t>(n);
        for (int frame = 0; frame < 6; ++frame) {
            std::vector<float> x(static_cast<std::size_t>(n));
            for (auto& v : x) {
                state = state * 1664525u + 1013904223u;
                v = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
            }
            const auto reference = reference_dft(std::vector<double>(x.begin(), x.end()));
            long double largest = 0;
            for (const auto& bin : reference)
                largest = std::max(largest, std::abs(bin));
            std::vector<std::complex<float>> bins(static_cast<std::size_t>(n / 2 + 1));
            fft.forward_real_half(x.data(), bins.data());
            for (std::size_t k = 0; k < bins.size(); ++k) {
                const std::complex<long double> y(bins[k].real(), bins[k].imag());
                worst = std::max(worst, static_cast<double>(std::abs(y - reference[k]) / largest));
            }
        }
        const double worst_db = 20.0 * std::log10(worst);
        INFO("n=" << n << " max relative error=" << worst_db << " dB");
#if PULP_FFT_HAS_VDSP
        REQUIRE(worst_db < -134.0);
#else
        REQUIRE(worst_db < -120.0);
#endif
    }
}
