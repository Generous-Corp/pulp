#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/signal.hpp>
#include <algorithm>
#include <cmath>
#include <complex>
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

// ── Convolver ────────────────────────────────────────────────────────────────

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
