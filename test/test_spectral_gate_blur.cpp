#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/spectral_gate_blur.hpp>

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>

using Catch::Matchers::WithinAbs;
using pulp::signal::SpectralFrameBlur;
using pulp::signal::SpectralGate;

namespace {

constexpr float kTolerance = 1.0e-6f;

void require_complex(std::complex<float> actual, std::complex<float> expected) {
    REQUIRE_THAT(actual.real(), WithinAbs(expected.real(), kTolerance));
    REQUIRE_THAT(actual.imag(), WithinAbs(expected.imag(), kTolerance));
}

} // namespace

TEST_CASE("Spectral gate applies scalar and per-bin magnitude thresholds independently",
          "[signal][spectral-gate][oracle]") {
    SpectralGate gate;
    REQUIRE(gate.set_threshold_magnitude(2.0f));

    std::array<std::complex<float>, 4> left = {
        std::complex<float>{0.6f, 0.8f},
        std::complex<float>{0.0f, 2.0f},
        std::complex<float>{-3.0f, 4.0f},
        std::complex<float>{1.0f, 0.0f},
    };
    std::array<std::complex<float>, 4> right = left;
    std::complex<float>* frames[] = {left.data(), right.data()};

    REQUIRE(gate.process(frames, 2, 4));
    require_complex(left[0], {});
    require_complex(left[1], {0.0f, 2.0f});
    require_complex(left[2], {-3.0f, 4.0f});
    require_complex(right[0], {});

    // This curve is the negative control for a scalar-only implementation:
    // equal-magnitude bins must split according to their individual threshold.
    left = {{{1.0f, 0.0f}, {1.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 0.0f}}};
    std::array<float, 4> thresholds = {0.5f, 1.5f, 1.5f, 2.5f};
    REQUIRE(gate.process(frames, 1, 4, thresholds.data()));
    require_complex(left[0], {1.0f, 0.0f});
    require_complex(left[1], {});
    require_complex(left[2], {2.0f, 0.0f});
    require_complex(left[3], {});
}

TEST_CASE("Spectral gate rejects invalid controls atomically and recovers hostile bins",
          "[signal][spectral-gate][fault]") {
    SpectralGate gate;
    REQUIRE(gate.set_threshold_magnitude(0.5f));
    REQUIRE_FALSE(gate.set_threshold_magnitude(-1.0f));
    REQUIRE_FALSE(gate.set_threshold_magnitude(
        std::numeric_limits<float>::quiet_NaN()));
    REQUIRE(gate.threshold_magnitude() == 0.5f);

    std::array<std::complex<float>, 2> bins = {
        std::complex<float>{1.0f, 0.0f}, std::complex<float>{2.0f, 0.0f}};
    std::complex<float>* frames[] = {bins.data()};
    std::array<float, 2> invalid = {
        0.0f, std::numeric_limits<float>::infinity()};
    REQUIRE_FALSE(gate.process(frames, 1, 2, invalid.data()));
    require_complex(bins[0], {1.0f, 0.0f});
    require_complex(bins[1], {2.0f, 0.0f});

    bins[0] = {std::numeric_limits<float>::infinity(), 0.0f};
    REQUIRE(gate.process(frames, 1, 2));
    require_complex(bins[0], {});
    require_complex(bins[1], {2.0f, 0.0f});
}

TEST_CASE("Spectral frame blur matches a hand-computed finite moving-average oracle",
          "[signal][spectral-frame-blur][oracle]") {
    SpectralFrameBlur blur;
    REQUIRE(blur.prepare(1, 2, 2));

    std::array<std::complex<float>, 2> bins;
    std::complex<float>* frames[] = {bins.data()};

    bins = {{{1.0f, 0.0f}, {0.0f, 2.0f}}};
    REQUIRE(blur.process(frames, 1, 2));
    require_complex(bins[0], {1.0f, 0.0f});
    require_complex(bins[1], {0.0f, 2.0f});

    bins = {{{0.0f, 3.0f}, {-4.0f, 0.0f}}};
    REQUIRE(blur.process(frames, 1, 2));
    require_complex(bins[0], {0.0f, 2.0f});
    require_complex(bins[1], {-3.0f, 0.0f});

    bins = {{{-5.0f, 0.0f}, {0.0f, -6.0f}}};
    REQUIRE(blur.process(frames, 1, 2));
    require_complex(bins[0], {-4.0f, 0.0f});
    require_complex(bins[1], {0.0f, -5.0f});
    REQUIRE(blur.filled_frames() == 2);
}

TEST_CASE("Spectral frame blur smears a vanished bin for only its bounded history",
          "[signal][spectral-frame-blur][negative-control]") {
    SpectralFrameBlur blur;
    REQUIRE(blur.prepare(1, 1, 3));

    std::complex<float> bin{0.0f, 3.0f};
    std::complex<float>* frames[] = {&bin};
    REQUIRE(blur.process(frames, 1, 1));
    require_complex(bin, {0.0f, 3.0f});

    bin = {};
    REQUIRE(blur.process(frames, 1, 1));
    require_complex(bin, {0.0f, 1.5f});
    bin = {};
    REQUIRE(blur.process(frames, 1, 1));
    require_complex(bin, {0.0f, 1.0f});
    bin = {};
    REQUIRE(blur.process(frames, 1, 1));
    require_complex(bin, {});

    // Neither an instantaneous pass-through nor an unbounded EMA produces the
    // exact finite sequence above and exact zero on frame four.
}

TEST_CASE("Spectral frame blur rejects bad geometry and resets deterministically",
          "[signal][spectral-frame-blur][lifecycle]") {
    SpectralFrameBlur blur;
    REQUIRE_FALSE(blur.prepare(0, 8, 4));
    REQUIRE_FALSE(blur.prepare(1, 8, SpectralFrameBlur::maximum_frames + 1));
    REQUIRE(blur.prepare(2, 3, 4));
    REQUIRE(blur.channels() == 2);
    REQUIRE(blur.num_bins() == 3);
    REQUIRE(blur.blur_frames() == 4);
    REQUIRE(blur.retained_bytes()
            == std::uint64_t{2 * 3 * 4 * sizeof(float)
                           + 2 * 3 * sizeof(float)
                           + 2 * 3 * sizeof(std::uint16_t)
                           + 2 * 3 * sizeof(std::complex<float>)});

    std::array<std::complex<float>, 3> left = {
        std::complex<float>{1.0f, 0.0f}, {2.0f, 0.0f}, {3.0f, 0.0f}};
    std::array<std::complex<float>, 3> right = {
        std::complex<float>{4.0f, 0.0f}, {5.0f, 0.0f}, {6.0f, 0.0f}};
    const auto expected_left = left;
    const auto expected_right = right;
    std::complex<float>* frames[] = {left.data(), right.data()};
    REQUIRE(blur.process(frames, 2, 3));
    blur.reset();
    left = expected_left;
    right = expected_right;
    REQUIRE(blur.process(frames, 2, 3));
    for (std::size_t i = 0; i < left.size(); ++i) {
        require_complex(left[i], expected_left[i]);
        require_complex(right[i], expected_right[i]);
    }

    const auto before = left;
    REQUIRE_FALSE(blur.process(frames, 1, 3));
    REQUIRE(left == before);
}

TEST_CASE("Spectral gate and frame blur allocate nothing after preparation",
          "[signal][spectral-gate][spectral-frame-blur][rt]") {
    SpectralGate gate;
    REQUIRE(gate.set_threshold_magnitude(0.25f));
    SpectralFrameBlur blur;
    REQUIRE(blur.prepare(2, 8, 4));

    std::array<std::complex<float>, 8> left{};
    std::array<std::complex<float>, 8> right{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = {static_cast<float>(i + 1), -0.25f};
        right[i] = {-0.5f, static_cast<float>(i + 1)};
    }
    std::complex<float>* frames[] = {left.data(), right.data()};

    bool processed = true;
    std::size_t allocation_count = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int frame = 0; frame < 32; ++frame) {
            processed = gate.process(frames, 2, 8) && processed;
            processed = blur.process(frames, 2, 8) && processed;
        }
        allocation_count = probe.allocation_count();
    }
    REQUIRE(processed);
    REQUIRE(allocation_count == 0);
}

TEST_CASE("Spectral frame blur contains non-finite history and recovers next frame",
          "[signal][spectral-frame-blur][fault]") {
    SpectralFrameBlur blur;
    REQUIRE(blur.prepare(1, 1, 1));

    std::complex<float> bin{std::numeric_limits<float>::quiet_NaN(), 1.0f};
    std::complex<float>* frames[] = {&bin};
    REQUIRE(blur.process(frames, 1, 1));
    require_complex(bin, {});

    bin = {3.0f, 4.0f};
    REQUIRE(blur.process(frames, 1, 1));
    require_complex(bin, {3.0f, 4.0f});
}

TEST_CASE("Spectral frame blur keeps a finite mean for large finite bins",
          "[signal][spectral-frame-blur][overflow]") {
    SpectralFrameBlur blur;
    REQUIRE(blur.prepare(1, 1, 2));

    const float large = std::numeric_limits<float>::max() * 0.75f;
    std::complex<float> bin{large, 0.0f};
    std::complex<float>* frames[] = {&bin};
    REQUIRE(blur.process(frames, 1, 1));
    REQUIRE(std::isfinite(bin.real()));
    bin = {large, 0.0f};
    REQUIRE(blur.process(frames, 1, 1));
    REQUIRE(std::isfinite(bin.real()));
    REQUIRE(bin.real() == large);
    REQUIRE(bin.imag() == 0.0f);

    blur.reset();
    bin = {large, large};
    REQUIRE(blur.process(frames, 1, 1));
    REQUIRE(std::isfinite(bin.real()));
    REQUIRE(std::isfinite(bin.imag()));
    REQUIRE(bin.real() > 0.0f);
    REQUIRE(bin.imag() == bin.real());
}
