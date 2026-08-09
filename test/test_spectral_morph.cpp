#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/spectral_morph.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>

using Catch::Matchers::WithinAbs;
using pulp::signal::SpectralMagnitudeInterpolation;
using pulp::signal::SpectralMorph;
using pulp::signal::SpectralPhaseInterpolation;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTolerance = 2.0e-5f;
constexpr int kBins = 129;
constexpr int kProbeBin = 17;

std::complex<float> polar(float magnitude, float degrees) {
    return std::polar(magnitude, degrees * kPi / 180.0f);
}

void require_complex(std::complex<float> actual, std::complex<float> expected,
                     float tolerance = kTolerance) {
    REQUIRE_THAT(actual.real(), WithinAbs(expected.real(), tolerance));
    REQUIRE_THAT(actual.imag(), WithinAbs(expected.imag(), tolerance));
}

float magnitude(std::complex<float> value) { return std::hypot(value.real(), value.imag()); }
float phase(std::complex<float> value) { return std::atan2(value.imag(), value.real()); }

} // namespace

TEST_CASE("Spectral morph linearly interpolates magnitude across the phase seam",
          "[signal][spectral-morph][oracle][wrap]") {
    SpectralMorph morph;
    REQUIRE(morph.prepare(1, kBins));

    std::array<std::complex<float>, kBins> a{}, b{}, out{};
    a[kProbeBin] = polar(2.0f, 179.0f);
    b[kProbeBin] = polar(6.0f, -179.0f);
    const std::complex<float>* ap[] = {a.data()};
    const std::complex<float>* bp[] = {b.data()};
    std::complex<float>* op[] = {out.data()};

    REQUIRE(morph.process(ap, bp, op, 1, kBins, 0.25f, 0.5f));
    REQUIRE_THAT(magnitude(out[kProbeBin]), WithinAbs(3.0f, kTolerance));
    REQUIRE_THAT(std::abs(phase(out[kProbeBin])), WithinAbs(kPi, kTolerance));
    REQUIRE(out[kProbeBin].real() < -2.99f);

    // Raw angle lerp is the negative control: (179 + -179) / 2 = 0 and would
    // point right. The wrap-safe result must point left across the short seam.
}

TEST_CASE("Spectral morph magnitude policies have independent analytic oracles",
          "[signal][spectral-morph][oracle][magnitude-policy]") {
    SpectralMorph morph;
    REQUIRE(morph.prepare(1, kBins));
    std::array<std::complex<float>, kBins> a{}, b{}, out{};
    a[kProbeBin] = {1.0f, 0.0f};
    b[kProbeBin] = {3.0f, 0.0f};
    const std::complex<float>* ap[] = {a.data()};
    const std::complex<float>* bp[] = {b.data()};
    std::complex<float>* op[] = {out.data()};

    REQUIRE(morph.process(ap, bp, op, 1, kBins, 0.5f, 0.5f));
    REQUIRE_THAT(magnitude(out[kProbeBin]), WithinAbs(2.0f, kTolerance));

    auto config = morph.config();
    config.magnitude = SpectralMagnitudeInterpolation::equal_power;
    REQUIRE(morph.set_config(config));
    REQUIRE(morph.process(ap, bp, op, 1, kBins, 0.5f, 0.5f));
    REQUIRE_THAT(magnitude(out[kProbeBin]), WithinAbs(std::sqrt(5.0f), kTolerance));
}

TEST_CASE("Spectral morph phase policies are independent of magnitude amount",
          "[signal][spectral-morph][oracle][phase-policy]") {
    SpectralMorph morph;
    REQUIRE(morph.prepare(1, kBins));
    std::array<std::complex<float>, kBins> a{}, b{}, out{};
    a[kProbeBin] = polar(2.0f, 0.0f);
    b[kProbeBin] = polar(6.0f, 120.0f);
    const std::complex<float>* ap[] = {a.data()};
    const std::complex<float>* bp[] = {b.data()};
    std::complex<float>* op[] = {out.data()};

    REQUIRE(morph.process(ap, bp, op, 1, kBins, 0.75f, 0.25f));
    REQUIRE_THAT(magnitude(out[kProbeBin]), WithinAbs(5.0f, kTolerance));
    REQUIRE_THAT(phase(out[kProbeBin]), WithinAbs(30.0f * kPi / 180.0f, kTolerance));

    auto config = morph.config();
    config.phase = SpectralPhaseInterpolation::normalized_vector;
    REQUIRE(morph.set_config(config));
    REQUIRE(morph.process(ap, bp, op, 1, kBins, 0.75f, 0.25f));
    const float vector_phase = std::atan2(0.25f * std::sin(120.0f * kPi / 180.0f),
                                          0.75f + 0.25f * std::cos(120.0f * kPi / 180.0f));
    REQUIRE_THAT(phase(out[kProbeBin]), WithinAbs(vector_phase, kTolerance));
    REQUIRE(std::abs(vector_phase - 30.0f * kPi / 180.0f) > 0.1f);

    a[kProbeBin] = polar(2.0f, 179.0f);
    b[kProbeBin] = polar(6.0f, -179.0f);
    REQUIRE(morph.process(ap, bp, op, 1, kBins, 0.5f, 0.5f));
    REQUIRE(out[kProbeBin].real() < -3.99f);
    REQUIRE_THAT(std::abs(phase(out[kProbeBin])), WithinAbs(kPi, kTolerance));
}

TEST_CASE("Spectral morph preserves exact endpoints and endpoint aliasing",
          "[signal][spectral-morph][endpoint][alias]") {
    SpectralMorph morph;
    REQUIRE(morph.prepare(1, kBins));
    std::array<std::complex<float>, kBins> a{};
    std::array<std::complex<float>, kBins> b{};
    a[0] = {1.25f, 0.0f};
    a[1] = {-3.0f, 4.5f};
    a[2] = {2.0f, -1.0f};
    a[kBins - 1] = {3.0f, 0.0f};
    b[0] = {-5.0f, 0.0f};
    b[1] = {7.0f, -8.0f};
    b[2] = {-2.5f, 6.0f};
    b[kBins - 1] = {-4.0f, 0.0f};
    const auto original_a = a;
    const auto original_b = b;
    const std::complex<float>* ap[] = {a.data()};
    const std::complex<float>* bp[] = {b.data()};
    std::complex<float>* op[] = {a.data()};

    REQUIRE(morph.process(ap, bp, op, 1, kBins, 0.0f, 0.0f));
    REQUIRE(a == original_a);
    REQUIRE(morph.process(ap, bp, op, 1, kBins, 1.0f, 1.0f));
    REQUIRE(a == original_b);
}

TEST_CASE("Spectral morph keeps DC and Nyquist real through frame synthesis",
          "[signal][spectral-morph][self-conjugate][integration]") {
    constexpr int fft_size = 256;
    constexpr int bins = fft_size / 2 + 1;
    SpectralMorph morph;
    REQUIRE(morph.prepare(1, bins));

    std::array<std::complex<float>, bins> a{};
    std::array<std::complex<float>, bins> b{};
    std::array<std::complex<float>, bins> out{};
    const std::complex<float>* ap[] = {a.data()};
    const std::complex<float>* bp[] = {b.data()};
    std::complex<float>* op[] = {out.data()};

    a[0] = {std::numeric_limits<float>::quiet_NaN(), 0.0f};
    b[0] = {0.0f, 2.0f};
    REQUIRE(morph.process(ap, bp, op, 1, bins, 0.0f, 0.0f));
    REQUIRE(std::isfinite(out[0].real()));
    REQUIRE(out[0].imag() == 0.0f);

    a.fill({});
    b.fill({});
    out.fill({});
    a[0] = {1.0f, 0.0f};
    b[0] = {-1.0f, 0.0f};
    a[bins - 1] = {-2.0f, 0.0f};
    b[bins - 1] = {2.0f, 0.0f};
    REQUIRE(morph.process(ap, bp, op, 1, bins, 0.5f, 0.5f));
    require_complex(out[0], {-1.0f, 0.0f});
    require_complex(out[bins - 1], {2.0f, 0.0f});

    pulp::signal::SpectralFrameEngineConfig config;
    config.fft_size = fft_size;
    config.analysis_hop = 64;
    config.max_synthesis_hop = 64;
    pulp::signal::SpectralFrameEngine engine;
    engine.prepare(config);
    engine.synthesize_frame(op, 64);
    engine.synthesize_frame(op, 64);
    REQUIRE(engine.available_output() == 64);
    std::array<float, 64> rendered{};
    float* rendered_ptr[] = {rendered.data()};
    engine.read_output(rendered_ptr, 64);
    float peak = 0.0f;
    for (float sample : rendered) peak = std::max(peak, std::abs(sample));
    REQUIRE(peak > 1.0e-5f);
}

TEST_CASE("Spectral morph partitioning is bit-identical to whole-frame processing",
          "[signal][spectral-morph][partition]") {
    SpectralMorph whole;
    SpectralMorph partitioned;
    REQUIRE(whole.prepare(2, kBins));
    REQUIRE(partitioned.prepare(2, kBins));
    SpectralMorph::Config config;
    config.magnitude = SpectralMagnitudeInterpolation::equal_power;
    config.phase = SpectralPhaseInterpolation::normalized_vector;
    REQUIRE(whole.set_config(config));
    REQUIRE(partitioned.set_config(config));

    std::array<std::complex<float>, kBins> a0{}, a1{}, b0{}, b1{};
    for (int bin = 0; bin < kBins; ++bin) {
        a0[bin] = polar(static_cast<float>(bin + 1), -170.0f + 31.0f * bin);
        a1[bin] = polar(static_cast<float>(bin + 2), 160.0f - 23.0f * bin);
        b0[bin] = polar(static_cast<float>(kBins + 1 - bin), 175.0f - 29.0f * bin);
        b1[bin] = polar(static_cast<float>(kBins + 2 - bin), -165.0f + 19.0f * bin);
    }
    const std::complex<float>* ap[] = {a0.data(), a1.data()};
    const std::complex<float>* bp[] = {b0.data(), b1.data()};
    std::array<std::complex<float>, kBins> whole0{}, whole1{}, split0{}, split1{};
    std::complex<float>* whole_out[] = {whole0.data(), whole1.data()};
    std::complex<float>* split_out[] = {split0.data(), split1.data()};

    REQUIRE(whole.process(ap, bp, whole_out, 2, kBins, 0.37f, 0.63f));
    REQUIRE(partitioned.process_partition(ap, bp, split_out, 2, 0, 53, 0.37f, 0.63f));
    REQUIRE(partitioned.process_partition(ap, bp, split_out, 2, 53, kBins - 53,
                                          0.37f, 0.63f));
    REQUIRE(whole0 == split0);
    REQUIRE(whole1 == split1);
}

TEST_CASE("Spectral morph rejects invalid lifecycle and configuration atomically",
          "[signal][spectral-morph][lifecycle][config]") {
    SpectralMorph morph;
    REQUIRE_FALSE(morph.prepare(0, kBins));
    REQUIRE_FALSE(morph.prepared());
    REQUIRE_FALSE(morph.prepare(1, SpectralMorph::minimum_bins - 1));
    REQUIRE(morph.prepare(2, kBins));
    REQUIRE(morph.channels() == 2);
    REQUIRE(morph.num_bins() == kBins);
    REQUIRE_FALSE(morph.prepare(1, SpectralMorph::maximum_bins + 1));
    REQUIRE(morph.channels() == 2);
    REQUIRE(morph.num_bins() == kBins);

    const auto before = morph.config();
    auto invalid = before;
    invalid.phase = static_cast<SpectralPhaseInterpolation>(99);
    REQUIRE_FALSE(morph.set_config(invalid));
    REQUIRE(morph.config().phase == before.phase);
    morph.reset();
    REQUIRE(morph.prepared());
}

TEST_CASE("Spectral morph contains non-finite endpoints and finite magnitude overflow",
          "[signal][spectral-morph][fault]") {
    SpectralMorph morph;
    REQUIRE(morph.prepare(1, kBins));
    const float large = std::numeric_limits<float>::max() * 0.75f;
    std::array<std::complex<float>, kBins> a{};
    std::array<std::complex<float>, kBins> b{};
    std::array<std::complex<float>, kBins> out{};
    a[1] = {std::numeric_limits<float>::quiet_NaN(), 0.0f};
    b[1] = {3.0f, 4.0f};
    a[2] = {std::numeric_limits<float>::infinity(), 0.0f};
    b[2] = {std::numeric_limits<float>::quiet_NaN(), 1.0f};
    a[3] = {large, large};
    b[3] = {large, large};
    const std::complex<float>* ap[] = {a.data()};
    const std::complex<float>* bp[] = {b.data()};
    std::complex<float>* op[] = {out.data()};

    REQUIRE(morph.process(ap, bp, op, 1, kBins, 0.5f, 0.5f));
    require_complex(out[1], {3.0f, 4.0f});
    require_complex(out[2], {});
    REQUIRE(std::isfinite(out[3].real()));
    REQUIRE(std::isfinite(out[3].imag()));
    REQUIRE(out[3].real() > 0.0f);
    REQUIRE(out[3].imag() == out[3].real());

    REQUIRE(morph.process(ap, bp, op, 1, kBins, 0.0f, 0.0f));
    REQUIRE(std::isfinite(magnitude(out[3])));
    REQUIRE(out[3].real() > 0.0f);
    REQUIRE(out[3].imag() == out[3].real());

    const auto before = out;
    REQUIRE_FALSE(morph.process(ap, bp, op, 1, kBins,
                                std::numeric_limits<float>::quiet_NaN(), 0.5f));
    REQUIRE(out == before);
}

TEST_CASE("Spectral morph processing allocates nothing",
          "[signal][spectral-morph][rt]") {
    SpectralMorph morph;
    REQUIRE(morph.prepare(2, kBins));
    std::array<std::complex<float>, kBins> a0{}, a1{}, b0{}, b1{}, out0{}, out1{};
    for (int bin = 0; bin < kBins; ++bin) {
        a0[bin] = polar(static_cast<float>(bin + 1), -170.0f + bin);
        a1[bin] = polar(static_cast<float>(bin + 2), 160.0f - bin);
        b0[bin] = polar(static_cast<float>(bin + 3), 170.0f - bin);
        b1[bin] = polar(static_cast<float>(bin + 4), -160.0f + bin);
    }
    const std::complex<float>* ap[] = {a0.data(), a1.data()};
    const std::complex<float>* bp[] = {b0.data(), b1.data()};
    std::complex<float>* op[] = {out0.data(), out1.data()};

    bool processed = true;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 32; ++i)
            processed = morph.process(ap, bp, op, 2, kBins, 0.4f, 0.6f) && processed;
        allocations = probe.allocation_count();
    }
    REQUIRE(processed);
    REQUIRE(allocations == 0);
}
