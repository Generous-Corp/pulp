// The finite spectral stream end-of-stream rule, checked against the analysis
// frame grid at several codec geometries rather than against its own formula.

#include "spectral_finite_stream_internal.hpp"

#include <pulp/signal/spectral_frame_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <set>
#include <vector>

using pulp::host::SpectralFiniteStreamRule;
using Status = SpectralFiniteStreamRule::Status;

namespace {

struct Geometry {
    std::uint64_t fft, hop, channels;
};

const std::vector<Geometry> geometries = {
    {256, 64, 1},   {256, 128, 1},   {512, 128, 1}, {1024, 256, 2},
    {2048, 128, 4}, {4096, 2048, 3}, {256, 1, 1},   {16384, 8192, 16},
};

// Count analysis frames whose window [s, s + N) overlaps [0, L), walking the
// same grid the frame engine starts on.
std::uint64_t grid_frames(std::uint64_t n, std::uint64_t h, std::uint64_t length) {
    auto start = pulp::signal::spectral_frame_engine_first_frame_start(static_cast<int>(n),
                                                                       static_cast<int>(h), true);
    std::uint64_t count = 0;
    for (; start < static_cast<std::int64_t>(length); start += static_cast<std::int64_t>(h))
        if (start + static_cast<std::int64_t>(n) > 0)
            ++count;
    return count;
}

std::int64_t last_window_end(const Geometry& g, std::uint64_t frames) {
    const auto first = pulp::signal::spectral_frame_engine_first_frame_start(
        static_cast<int>(g.fft), static_cast<int>(g.hop), true);
    return first + static_cast<std::int64_t>((frames - 1) * g.hop + g.fft);
}

} // namespace

static_assert(SpectralFiniteStreamRule::make(256, 64, 1, 769).frame_count == 16);

TEST_CASE("Finite stream rule matches the profile-one asset constants", "[spectral][eof]") {
    std::vector<std::uint64_t> lengths;
    for (std::uint64_t l = 1; l <= 4096; ++l)
        lengths.push_back(l);
    for (std::uint64_t l : {65537ull, 1000003ull, 48000ull * 3600ull})
        lengths.push_back(l);
    for (const auto l : lengths) {
        const auto rule = SpectralFiniteStreamRule::make(256, 64, 1, l);
        REQUIRE(rule);
        const auto frames = (l + 192 + 63) / 64;
        REQUIRE(rule.frame_count == frames);
        REQUIRE(rule.render_cutoff == 320 + frames * 64);
        REQUIRE(rule.finite_frames == frames + 4);
        REQUIRE(rule.bins == 129);
        REQUIRE(rule.frame_stride() == 258);
        REQUIRE(rule.component(frames - 1, 0, 128) == (frames - 1) * 258 + 256);
    }
}

TEST_CASE("Finite stream frame count equals the analysis grid at every geometry",
          "[spectral][eof]") {
    for (const auto& g : geometries) {
        CAPTURE(g.fft, g.hop, g.channels);
        std::vector<std::uint64_t> lengths = {1,     2,         g.hop,         g.hop + 1, g.fft - 1,
                                              g.fft, g.fft + 1, 3 * g.fft + 7, 104729};
        for (std::uint64_t l = 1; l <= 2 * g.hop + 3; ++l)
            lengths.push_back(l);
        for (const auto l : lengths) {
            CAPTURE(l);
            const auto rule = SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, l);
            REQUIRE(rule);
            REQUIRE(rule.frame_count == grid_frames(g.fft, g.hop, l));
            // The cutoff is one window plus one hop past the last frame's window,
            // and the published count reaches one hop before the cutoff.
            REQUIRE(static_cast<std::int64_t>(rule.render_cutoff) ==
                    last_window_end(g, rule.frame_count) +
                        static_cast<std::int64_t>(g.fft + g.hop));
            REQUIRE(rule.finite_frames * g.hop + g.hop == rule.render_cutoff);
            REQUIRE(rule.source_samples == l);
        }
    }
}

TEST_CASE("Finite stream component layout tiles frames, channels and bins", "[spectral][eof]") {
    for (const auto& g : geometries) {
        if (g.fft > 2048)
            continue;
        CAPTURE(g.fft, g.hop, g.channels);
        const auto rule = SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, 1);
        REQUIRE(rule);
        REQUIRE(rule.bins == g.fft / 2 + 1);
        std::set<std::uint64_t> offsets;
        for (std::uint64_t f = 0; f < 3; ++f)
            for (std::uint64_t c = 0; c < g.channels; ++c)
                for (std::uint64_t k = 0; k < rule.bins; ++k)
                    REQUIRE(offsets.insert(rule.component(f, c, k)).second);
        REQUIRE(offsets.size() * 2 == 3 * rule.frame_stride());
        REQUIRE(*offsets.begin() == 0);
        REQUIRE(*offsets.rbegin() == 3 * rule.frame_stride() - 2);
        REQUIRE(rule.component(0, g.channels - 1, 0) == (g.channels - 1) * rule.bins * 2);
    }
}

TEST_CASE("Finite stream rule refuses inadmissible geometry", "[spectral][eof]") {
    const Geometry refused[] = {{128, 64, 1},  {32768, 64, 1}, {300, 64, 1}, {256, 0, 1},
                                {256, 129, 1}, {256, 100, 1},  {256, 64, 0}, {256, 64, 17}};
    for (const auto& g : refused) {
        CAPTURE(g.fft, g.hop, g.channels);
        const auto rule = SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, 769);
        REQUIRE_FALSE(rule);
        REQUIRE(rule.status == Status::invalid_geometry);
    }
    // Control: the same lengths at an admitted neighbour geometry are accepted.
    REQUIRE(SpectralFiniteStreamRule::make(256, 128, 1, 769));
    REQUIRE(SpectralFiniteStreamRule::make(256, 64, 16, 769));
}

TEST_CASE("A hop that does not divide the FFT breaks the closed-form count", "[spectral][eof]") {
    // This is why such hops are refused: ceil((L + N - H) / H) is not the
    // number of grid frames overlapping the source.
    const std::uint64_t n = 256, h = 100, l = 1;
    REQUIRE((l + n - h + h - 1) / h == 2);
    REQUIRE(grid_frames(n, h, l) == 3);
}

TEST_CASE("Finite stream rule bounds length and capacity against signed chronology",
          "[spectral][eof]") {
    constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    for (const auto& g : geometries) {
        CAPTURE(g.fft, g.hop, g.channels);
        REQUIRE(SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, 0).status ==
                Status::invalid_length);
        const auto too_long = limit - (g.fft - g.hop) - (g.hop - 1) + 1;
        REQUIRE(SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, too_long).status ==
                Status::invalid_length);
        REQUIRE(SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, too_long - 1).status ==
                Status::capacity_exceeded);
        const auto max_frames = (limit - (g.fft + g.hop) - g.fft) / g.hop;
        const auto longest = max_frames * g.hop - (g.fft - g.hop);
        const auto rule = SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, longest);
        REQUIRE(rule);
        REQUIRE(rule.frame_count == max_frames);
        REQUIRE(rule.render_cutoff <= limit - g.fft);
        REQUIRE(SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, longest + 1).status ==
                Status::capacity_exceeded);
    }
}
