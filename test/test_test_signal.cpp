#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/test_signal.hpp>
#include <cmath>
#include <vector>

using namespace pulp::format;
using Catch::Matchers::WithinAbs;

TEST_CASE("TestSignalSource: inactive produces silence", "[format][test_signal]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);

    constexpr int frames = 64;
    constexpr int channels = 2;
    std::vector<float> buf(channels * frames, 1.0f);
    float* ptrs[channels] = { buf.data(), buf.data() + frames };

    src.fill(ptrs, channels, frames);

    for (int i = 0; i < channels * frames; ++i) {
        REQUIRE(buf[static_cast<size_t>(i)] == 0.0f);
    }
}

TEST_CASE("TestSignalSource: sine tone produces non-zero output", "[format][test_signal]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);
    src.set_config({ TestSignalType::sine, 440.0f, 0.5f });

    constexpr int frames = 256;
    constexpr int channels = 2;
    std::vector<float> buf(channels * frames, 0.0f);
    float* ptrs[channels] = { buf.data(), buf.data() + frames };

    src.fill(ptrs, channels, frames);

    // Should have non-zero energy
    float energy = 0;
    for (int i = 0; i < frames; ++i) {
        energy += ptrs[0][i] * ptrs[0][i];
    }
    REQUIRE(energy > 0.1f);

    // Both channels should be identical (mono sine duplicated)
    for (int i = 0; i < frames; ++i) {
        REQUIRE_THAT(ptrs[0][i], WithinAbs(ptrs[1][i], 0.0001f));
    }

    // Peak should not exceed amplitude
    for (int i = 0; i < frames; ++i) {
        REQUIRE(std::abs(ptrs[0][i]) <= 0.51f);
    }
}

TEST_CASE("TestSignalSource: sine frequency is correct", "[format][test_signal]") {
    TestSignalSource src;
    double sr = 48000.0;
    src.set_sample_rate(sr);
    float freq = 1000.0f;
    src.set_config({ TestSignalType::sine, freq, 1.0f });

    // Generate one full period worth of samples
    int period_samples = static_cast<int>(sr / freq);
    std::vector<float> buf(static_cast<size_t>(period_samples), 0.0f);
    float* ptr = buf.data();

    src.fill(&ptr, 1, period_samples);

    // First sample should be near zero (sin(0))
    REQUIRE_THAT(buf[0], WithinAbs(0.0f, 0.01f));

    // Quarter-period sample should be near +1.0 (sin(π/2))
    int quarter = period_samples / 4;
    REQUIRE_THAT(buf[static_cast<size_t>(quarter)], WithinAbs(1.0f, 0.05f));
}

TEST_CASE("TestSignalSource: config change takes effect", "[format][test_signal]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);

    // Start with sine
    src.set_config({ TestSignalType::sine, 440.0f, 0.5f });
    REQUIRE(src.is_active());

    // Switch to none
    src.set_config({ TestSignalType::none, 440.0f, 0.5f });

    constexpr int frames = 64;
    std::vector<float> buf(static_cast<size_t>(frames), 1.0f);
    float* ptr = buf.data();
    src.fill(&ptr, 1, frames);

    // Should be silence after switching to none
    float energy = 0;
    for (float s : buf) energy += s * s;
    REQUIRE(energy == 0.0f);
    REQUIRE(!src.is_active());
}

TEST_CASE("TestSignalSource: reset clears state", "[format][test_signal]") {
    TestSignalSource src;
    src.set_sample_rate(48000.0);
    src.set_config({ TestSignalType::sine, 440.0f, 0.5f });

    // Generate some samples to advance phase
    constexpr int frames = 256;
    std::vector<float> buf1(static_cast<size_t>(frames));
    std::vector<float> buf2(static_cast<size_t>(frames));
    float* ptr1 = buf1.data();
    float* ptr2 = buf2.data();

    src.fill(&ptr1, 1, frames);
    src.reset();

    // After reset + re-enable, output should start from phase 0 again
    src.set_config({ TestSignalType::sine, 440.0f, 0.5f });
    src.fill(&ptr2, 1, frames);

    // First sample should be near zero (phase reset)
    REQUIRE_THAT(buf2[0], WithinAbs(0.0f, 0.01f));
}
