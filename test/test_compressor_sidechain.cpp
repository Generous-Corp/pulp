#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/compressor.hpp>

#include <cmath>
#include <vector>

using namespace pulp::signal;

namespace {

float db_to_lin(float db) { return std::pow(10.0f, db / 20.0f); }

float rms(const std::vector<float>& v, int start, int end) {
    double acc = 0.0;
    const int n = end - start;
    for (int i = start; i < end; ++i) acc += double(v[i]) * double(v[i]);
    return static_cast<float>(std::sqrt(acc / n));
}

} // namespace

TEST_CASE("Compressor latency_samples reflects lookahead",
          "[signal][compressor]") {
    Compressor c;
    c.set_sample_rate(48000.0f);
    REQUIRE(c.latency_samples() == 0);

    c.set_lookahead_ms(1.0f);
    REQUIRE(c.latency_samples() == 48);

    c.set_lookahead_ms(5.0f);
    REQUIRE(c.latency_samples() == 240);

    // Clamp at 50 ms max.
    c.set_lookahead_ms(1000.0f);
    REQUIRE(c.lookahead_ms() == 50.0f);
    REQUIRE(c.latency_samples() == 2400);
}

TEST_CASE("Compressor with lookahead delays the dry signal by the same amount",
          "[signal][compressor]") {
    Compressor c;
    c.set_sample_rate(48000.0f);
    c.set_lookahead_ms(1.0f); // 48 samples
    Compressor::Params p; p.threshold_db = 0.0f; // no actual compression
    c.set_params(p);

    // Drive with an impulse at sample 0.
    std::vector<float> buf(256, 0.0f);
    buf[0] = 1.0f;
    c.process(buf.data(), 256);

    // With lookahead = 48 samples, the impulse should appear at
    // sample ~48 in the output (the dry signal was delayed by the
    // lookahead amount).
    int found = -1;
    for (int i = 0; i < 256; ++i) {
        if (std::abs(buf[i]) > 0.5f) { found = i; break; }
    }
    REQUIRE(found == 48);
}

TEST_CASE("Compressor sidechain HPF attenuates bass-driven triggering",
          "[signal][compressor]") {
    // A loud sub-bass sidechain signal should NOT cause gain reduction
    // when the HPF is set above the sub-bass.
    Compressor c;
    c.set_sample_rate(48000.0f);
    c.set_sidechain_hpf_hz(200.0f);
    Compressor::Params p;
    p.threshold_db = -20.0f;
    p.ratio = 8.0f;
    p.attack_ms = 0.1f;
    p.release_ms = 50.0f;
    c.set_params(p);

    const int n = 4096;
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kSub = 60.0f; // well below the 200 Hz HPF corner

    // Build a hot 60 Hz sidechain.
    std::vector<float> sidechain(n);
    for (int i = 0; i < n; ++i)
        sidechain[i] = 0.8f * std::sin(kTwoPi * kSub * float(i) / 48000.0f);

    // Process a constant 0.5 input against the bass sidechain.
    std::vector<float> input(n, 0.5f);
    c.process_with_sidechain(input.data(), sidechain.data(), n);

    // With HPF on, the sidechain detector sees a much-attenuated bass,
    // so envelope_db should hover close to 0 → almost no gain reduction
    // → output ≈ 0.5.
    const float tail_rms = rms(input, n / 2, n);
    REQUIRE(tail_rms > 0.45f); // very little compression
    REQUIRE(c.gain_reduction_db() > -3.0f); // shallow at worst
}

TEST_CASE("Compressor sidechain WITHOUT HPF clearly compresses bass-driven input",
          "[signal][compressor]") {
    Compressor c;
    c.set_sample_rate(48000.0f);
    c.set_sidechain_hpf_hz(0.0f); // HPF disabled
    Compressor::Params p;
    p.threshold_db = -20.0f;
    p.ratio = 8.0f;
    p.attack_ms = 0.1f;
    p.release_ms = 50.0f;
    c.set_params(p);

    const int n = 4096;
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kSub = 60.0f;

    std::vector<float> sidechain(n);
    for (int i = 0; i < n; ++i)
        sidechain[i] = 0.8f * std::sin(kTwoPi * kSub * float(i) / 48000.0f);

    std::vector<float> input(n, 0.5f);
    c.process_with_sidechain(input.data(), sidechain.data(), n);

    // Without HPF the bass triggers compression → audible gain reduction.
    REQUIRE(c.gain_reduction_db() < -3.0f);
}

TEST_CASE("Compressor process(input) still works as a self-sidechain compressor",
          "[signal][compressor]") {
    // The single-arg `process(input)` API must keep working with input acting
    // as its own sidechain detector.
    Compressor c;
    c.set_sample_rate(48000.0f);
    Compressor::Params p;
    p.threshold_db = -20.0f;
    p.ratio = 4.0f;
    p.attack_ms = 5.0f;
    p.release_ms = 50.0f;
    c.set_params(p);

    // Drive with a -10 dBFS sine (above threshold) and assert gain
    // reduction kicks in.
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float amp = db_to_lin(-10.0f);
    std::vector<float> buf(4096);
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = amp * std::sin(kTwoPi * 1000.0f * float(i) / 48000.0f);
    c.process(buf.data(), static_cast<int>(buf.size()));

    REQUIRE(c.gain_reduction_db() < -1.0f);
}

TEST_CASE("Compressor reset clears sidechain HPF + lookahead state",
          "[signal][compressor]") {
    Compressor c;
    c.set_sample_rate(48000.0f);
    c.set_sidechain_hpf_hz(150.0f);
    c.set_lookahead_ms(2.0f);
    Compressor::Params p; p.threshold_db = -20.0f;
    c.set_params(p);

    // Pump some signal through to populate the delay + filter state.
    for (int i = 0; i < 512; ++i) (void) c.process(0.5f);

    c.reset();
    // After reset, processing a zero impulse stream should produce
    // zeros for at least the lookahead window (delay line cleared).
    bool all_zero = true;
    for (int i = 0; i < c.latency_samples(); ++i) {
        if (std::abs(c.process(0.0f)) > 1e-9f) { all_zero = false; break; }
    }
    REQUIRE(all_zero);
}
