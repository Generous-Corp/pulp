// MultiChannelMeter block processing: the vector kernels against the
// per-sample definition, the feature set, and the over-range path.
//
// process() measures a clean piece with vector reductions and falls back to
// the sanitizing per-sample loop for any piece holding a non-finite or
// over-range sample. Both must report the same readings. The parity cases
// drive the two paths through the public API: a programme with exact zeros
// takes the vector path, and the same programme with NaN in place of those
// zeros takes the per-sample path for every piece that holds one, because NaN
// is defined to read as silence.

#include <pulp/signal/multi_channel_meter.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace pulp::signal;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;

struct Programme {
    std::vector<float> left;
    std::vector<float> right;
};

// Noise plus a tone, partly correlated across the rails, with an exact zero
// in every block at an offset that walks through the block.
Programme programme(int seconds) {
    const auto n = static_cast<std::size_t>(kRate) * static_cast<std::size_t>(seconds);
    Programme p{std::vector<float>(n), std::vector<float>(n)};
    std::uint32_t state = 0x2545F491u;
    auto noise = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(static_cast<double>(state >> 8) / 8388608.0 - 1.0);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const float tone = 0.2f * static_cast<float>(std::sin(6.283185307179586 * 997.0 *
                                                              static_cast<double>(i) / kRate));
        // The level steps every second so the gated loudness has a programme.
        const float level = (i / static_cast<std::size_t>(kRate)) % 2 == 0 ? 0.25f : 0.05f;
        const float common = noise();
        p.left[i] = level * (0.7f * common + 0.3f * noise()) + tone;
        p.right[i] = level * (0.7f * common + 0.3f * noise()) - 0.5f * tone;
    }
    for (std::size_t block = 0; block * kBlock < n; ++block) {
        const std::size_t at = block * kBlock + (block * 7) % kBlock;
        if (at < n) {
            p.left[at] = 0.0f;
            p.right[at] = 0.0f;
        }
    }
    return p;
}

// The same programme with NaN wherever `programme` placed its zeros.
Programme with_nan_at_zeros(Programme p, float replacement) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (std::size_t block = 0; block * kBlock < p.left.size(); ++block) {
        const std::size_t at = block * kBlock + (block * 7) % kBlock;
        if (at < p.left.size()) {
            p.left[at] = std::isnan(replacement) ? nan : replacement;
            p.right[at] = std::isnan(replacement) ? nan : replacement;
        }
    }
    return p;
}

struct Readings {
    float peak[2];
    float rms[2];
    float correlation;
    float momentary;
    float integrated;
    float short_term;
    float range;
};

Readings run(const Programme& p, MeterFeatures features = MeterFeatures::all) {
    MultiChannelMeter meter;
    meter.prepare(kRate, 2, nullptr, features);
    for (std::size_t offset = 0; offset < p.left.size(); offset += kBlock) {
        const int n = static_cast<int>(std::min<std::size_t>(kBlock, p.left.size() - offset));
        const float* channels[] = {p.left.data() + offset, p.right.data() + offset};
        meter.process(channels, 2, n);
    }
    const auto& s = meter.snapshot();
    return {{s.channels[0].peak, s.channels[1].peak},
            {s.channels[0].rms, s.channels[1].rms},
            s.correlation,
            s.lufs_momentary,
            s.lufs_integrated,
            s.short_term_lufs,
            s.loudness_range_lu};
}

double worst_difference(const Readings& a, const Readings& b) {
    double worst = 0.0;
    auto track = [&](double x, double y) { worst = std::max(worst, std::abs(x - y)); };
    for (int ch = 0; ch < 2; ++ch) {
        track(a.peak[ch], b.peak[ch]);
        track(a.rms[ch], b.rms[ch]);
    }
    track(a.correlation, b.correlation);
    track(a.momentary, b.momentary);
    track(a.integrated, b.integrated);
    track(a.short_term, b.short_term);
    track(a.range, b.range);
    return worst;
}

} // namespace

TEST_CASE("MultiChannelMeter vector and per-sample paths report the same readings",
          "[signal][meter][parity]") {
    const auto clean = programme(6);
    const auto vector_path = run(clean);
    // NaN reads as silence, so this is the same programme measured through
    // the per-sample path in every block.
    const auto per_sample_path = run(with_nan_at_zeros(clean, std::nanf("")));

    INFO("integrated " << vector_path.integrated << " vs " << per_sample_path.integrated << ", LRA "
                       << vector_path.range << " vs " << per_sample_path.range);
    REQUIRE(std::isfinite(vector_path.integrated));
    REQUIRE(std::isfinite(vector_path.short_term));
    REQUIRE(vector_path.range > 1.0f);
    for (int ch = 0; ch < 2; ++ch) {
        REQUIRE(vector_path.peak[ch] == per_sample_path.peak[ch]);
        REQUIRE_THAT(vector_path.rms[ch], WithinRel(per_sample_path.rms[ch], 1e-6f));
    }
    REQUIRE_THAT(vector_path.correlation, WithinAbs(per_sample_path.correlation, 1e-6));
    REQUIRE_THAT(vector_path.momentary, WithinAbs(per_sample_path.momentary, 1e-4));
    REQUIRE_THAT(vector_path.integrated, WithinAbs(per_sample_path.integrated, 1e-4));
    REQUIRE_THAT(vector_path.short_term, WithinAbs(per_sample_path.short_term, 1e-4));
    REQUIRE_THAT(vector_path.range, WithinAbs(per_sample_path.range, 1e-4));
}

TEST_CASE("MultiChannelMeter path parity comparison detects a changed programme",
          "[signal][meter][parity]") {
    // Negative control for the case above: the same comparison, but the
    // per-sample run sees a real value where the vector run saw silence. If
    // this difference vanished, the parity case could not fail either.
    const auto clean = programme(6);
    const auto changed = run(with_nan_at_zeros(clean, 0.5f));
    REQUIRE(worst_difference(run(clean), changed) > 1e-3);
}

TEST_CASE("MultiChannelMeter levels-only measures the same levels without loudness",
          "[signal][meter][features]") {
    const auto clean = programme(3);
    const auto all = run(clean);
    const auto levels = run(clean, MeterFeatures::levels);
    for (int ch = 0; ch < 2; ++ch) {
        // Loudness splits the block at hop boundaries, which only reorders
        // the energy sums.
        REQUIRE(levels.peak[ch] == all.peak[ch]);
        REQUIRE_THAT(levels.rms[ch], WithinRel(all.rms[ch], 1e-6f));
    }
    REQUIRE(levels.correlation == 0.0f);
    REQUIRE(std::isinf(levels.momentary));
    REQUIRE(std::isinf(levels.integrated));
    // ... and the full meter really computed them, so the empty values above
    // are the feature set at work rather than a programme too quiet to gate.
    REQUIRE(std::isfinite(all.integrated));
    REQUIRE(std::abs(all.correlation) > 0.01f);

    MultiChannelMeter meter;
    meter.prepare(kRate, 2, nullptr, MeterFeatures::levels);
    REQUIRE_FALSE(meter.loudness_supported());
    REQUIRE(meter.features() == MeterFeatures::levels);
    meter.prepare(kRate, 2, nullptr, MeterFeatures::correlation);
    REQUIRE_FALSE(meter.loudness_supported());
    const auto correlation_only = run(clean, MeterFeatures::correlation);
    REQUIRE_THAT(correlation_only.correlation, WithinAbs(all.correlation, 1e-6));

    // A prepare() without a feature set measures everything again, whatever
    // an earlier call selected.
    meter.prepare(kRate, 2, nullptr);
    REQUIRE(meter.features() == MeterFeatures::all);
    REQUIRE(meter.loudness_supported());
    meter.prepare(kRate, 2, nullptr, MeterFeatures::levels);
    meter.prepare(kRate, 2);
    REQUIRE(meter.features() == MeterFeatures::all);
}

TEST_CASE("MultiChannelMeter clamps an over-range float sample inside a block", "[signal][meter]") {
    // One sample far outside the supported domain sends its piece down the
    // per-sample path, which clamps it to kMaxMeterInputMagnitude; the rest of
    // the block still counts.
    MultiChannelMeter meter;
    meter.prepare(1000.0, 1, nullptr, MeterFeatures::levels); // 10-sample snapshots
    std::vector<float> block(10, 0.5f);
    block[3] = 4.0e6f;
    const float* channels[] = {block.data()};
    meter.process(channels, 1, 10);
    const auto& snap = meter.snapshot();
    REQUIRE(snap.channels[0].peak == static_cast<float>(kMaxMeterInputMagnitude));
    REQUIRE(snap.channels[0].clipped);
    const double expected =
        std::sqrt((kMaxMeterInputMagnitude * kMaxMeterInputMagnitude + 9.0 * 0.25) / 10.0);
    REQUIRE_THAT(snap.channels[0].rms, WithinRel(static_cast<float>(expected), 1e-6f));
}
