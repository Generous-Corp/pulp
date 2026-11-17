// The finite spectral stream end-of-stream rule, checked against the analysis
// frame grid and the synthesis engine at several codec geometries rather than
// against its own formula.

#include "spectral_finite_stream_internal.hpp"

#include <pulp/signal/spectral_frame_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <complex>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

using pulp::host::SpectralFiniteStreamRule;
using Status = SpectralFiniteStreamRule::Status;
using Policy = SpectralFiniteStreamRule::Policy;

namespace {

struct Geometry {
    std::uint64_t fft, hop, channels;
};

const std::vector<Geometry> dividing = {
    {256, 64, 1},   {256, 128, 1},   {512, 128, 1}, {1024, 256, 2},
    {2048, 128, 4}, {4096, 2048, 3}, {256, 1, 1},   {16384, 8192, 16},
};
const std::vector<Geometry> non_dividing = {
    {256, 3, 1},  {256, 63, 1},    {256, 100, 2},  {512, 96, 1},
    {1024, 3, 1}, {2048, 1000, 3}, {16384, 24, 1}, {16384, 8191, 16},
};
std::vector<Geometry> all_geometries() {
    auto all = dividing;
    all.insert(all.end(), non_dividing.begin(), non_dividing.end());
    return all;
}
const std::vector<Geometry> geometries = all_geometries();

// Smallest number of frames after the last stored one whose start reaches the
// end of the last stored window.
std::uint64_t walked_drain(const Geometry& g) {
    std::uint64_t d = 0;
    while (d * g.hop < g.fft)
        ++d;
    return d;
}

std::vector<std::uint64_t> boundary_lengths(const Geometry& g) {
    std::vector<std::uint64_t> lengths = {1,     2,         g.hop,         g.hop + 1, g.fft - 1,
                                          g.fft, g.fft + 1, 3 * g.fft + 7, 104729};
    for (std::uint64_t l = 1; l <= 2 * g.hop + 3; ++l)
        lengths.push_back(l);
    return lengths;
}

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

// Stream positions the guarded synthesis engine has made final after
// `frames` frames, measured by reading until it underruns.
std::int64_t engine_final_positions(const Geometry& g, std::uint64_t frames) {
    pulp::signal::SpectralFrameEngineConfig cfg;
    cfg.fft_size = static_cast<int>(g.fft);
    cfg.analysis_hop = static_cast<int>(g.hop);
    cfg.channels = 1;
    cfg.max_block = 4096;
    cfg.max_synthesis_hop = static_cast<int>(g.hop);
    cfg.defer_synthesis = false;
    cfg.guarded_split = true;
    pulp::signal::SpectralFrameEngine engine;
    engine.prepare(cfg);
    const std::vector<std::complex<float>> silent(g.fft / 2 + 1);
    const std::complex<float>* frame[] = {silent.data()};
    std::int64_t read = 0;
    float sample = 0;
    float* out[] = {&sample};
    for (std::uint64_t f = 0; f < frames; ++f) {
        REQUIRE(engine.synthesize_frame_guarded(frame) == pulp::signal::SpectralSplitResult::Ok);
        int consumed = 0;
        while (engine.read_output_guarded(out, 1, consumed) ==
               pulp::signal::SpectralSplitResult::Ok)
            ++read;
    }
    return read;
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
        for (const auto l : boundary_lengths(g)) {
            CAPTURE(l);
            const auto rule = SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, l);
            REQUIRE(rule);
            REQUIRE(rule.policy == Policy::analysis_grid_v2);
            REQUIRE(rule.policy_version() == 2);
            REQUIRE(rule.frame_count == grid_frames(g.fft, g.hop, l));
            // The cutoff is one window plus one hop past the last frame's window.
            REQUIRE(static_cast<std::int64_t>(rule.render_cutoff) ==
                    last_window_end(g, rule.frame_count) +
                        static_cast<std::int64_t>(g.fft + g.hop));
            REQUIRE(rule.drain_frames == walked_drain(g));
            REQUIRE(rule.finite_frames == rule.frame_count + rule.drain_frames);
            REQUIRE(rule.source_samples == l);
        }
    }
}

TEST_CASE("Finite stream drain is the least the synthesis engine needs to finish the stream",
          "[spectral][eof]") {
    for (const auto& g : geometries) {
        if (g.fft > 4096)
            continue;
        CAPTURE(g.fft, g.hop);
        for (const std::uint64_t l : {std::uint64_t{1}, g.hop + 1, g.fft + 3}) {
            CAPTURE(l);
            const auto rule = SpectralFiniteStreamRule::make(g.fft, g.hop, 1, l);
            REQUIRE(rule);
            const auto end = last_window_end(g, rule.frame_count);
            // With the drain every sample of the last window is final; one
            // frame fewer leaves the window's tail unread.
            REQUIRE(engine_final_positions(g, rule.finite_frames) >= end);
            REQUIRE(engine_final_positions(g, rule.finite_frames - 1) < end);
        }
    }
}

TEST_CASE("Both finite stream policies keep the dividing-hop results bit for bit",
          "[spectral][eof]") {
    // The closed forms the dividing-hop rule has always produced.
    for (const auto& g : dividing) {
        CAPTURE(g.fft, g.hop, g.channels);
        auto lengths = boundary_lengths(g);
        for (std::uint64_t l : {65537ull, 1000003ull, 48000ull * 3600ull})
            lengths.push_back(l);
        for (const auto l : lengths) {
            CAPTURE(l);
            const auto v1 = SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, l,
                                                           Policy::dividing_hop_v1);
            const auto v2 = SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, l,
                                                           Policy::analysis_grid_v2);
            REQUIRE(v1);
            REQUIRE(v2);
            REQUIRE(v1.policy_version() == 1);
            const auto frames = (l + g.fft - g.hop + g.hop - 1) / g.hop;
            for (const auto& rule : {v1, v2}) {
                REQUIRE(rule.frame_count == frames);
                REQUIRE(rule.drain_frames == g.fft / g.hop);
                REQUIRE(rule.finite_frames == frames + g.fft / g.hop);
                REQUIRE(rule.render_cutoff == g.fft + g.hop + frames * g.hop);
                REQUIRE(rule.bins == v1.bins);
                REQUIRE(rule.source_samples == l);
            }
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
    struct Refused {
        Geometry geometry;
        Status status;
    };
    const Refused refused[] = {{{128, 64, 1}, Status::fft_size_not_admitted},
                               {{32768, 64, 1}, Status::fft_size_not_admitted},
                               {{300, 64, 1}, Status::fft_size_not_admitted},
                               {{256, 0, 1}, Status::hop_not_admitted},
                               {{256, 129, 1}, Status::hop_not_admitted},
                               {{256, 64, 0}, Status::channels_not_admitted},
                               {{256, 64, 17}, Status::channels_not_admitted}};
    for (const auto& [g, status] : refused) {
        CAPTURE(g.fft, g.hop, g.channels);
        const auto rule = SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, 769);
        REQUIRE_FALSE(rule);
        REQUIRE(rule.status == status);
        REQUIRE(rule.geometry_refused());
        REQUIRE_FALSE(SpectralFiniteStreamRule::admissible_geometry(g.fft, g.hop, g.channels));
    }
    // The dividing-hop policy alone refuses a hop that does not divide N; the
    // analysis-grid policy admits the same geometry.
    for (const auto& g : non_dividing) {
        CAPTURE(g.fft, g.hop, g.channels);
        const auto v1 =
            SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, 769, Policy::dividing_hop_v1);
        REQUIRE(v1.status == Status::hop_does_not_divide_fft_size);
        REQUIRE(v1.geometry_refused());
        REQUIRE(SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, 769,
                                               Policy::analysis_grid_v2));
    }
    const auto unknown = SpectralFiniteStreamRule::make(256, 64, 1, 769, static_cast<Policy>(3));
    REQUIRE(unknown.status == Status::policy_not_admitted);
    REQUIRE(unknown.geometry_refused());
    REQUIRE_FALSE(SpectralFiniteStreamRule::make(256, 64, 1, 769, static_cast<Policy>(0)));
    // Every refusal has its own name.
    std::set<std::string> names;
    for (const auto status :
         {Status::ok, Status::unset, Status::fft_size_not_admitted, Status::hop_not_admitted,
          Status::hop_does_not_divide_fft_size, Status::channels_not_admitted,
          Status::invalid_length, Status::capacity_exceeded, Status::policy_not_admitted})
        REQUIRE(names.insert(SpectralFiniteStreamRule::reason(status)).second);
    REQUIRE_FALSE(SpectralFiniteStreamRule::make(256, 64, 1, 0).geometry_refused());
    // Control: the same lengths at an admitted neighbour geometry are accepted.
    REQUIRE(SpectralFiniteStreamRule::make(256, 128, 1, 769));
    REQUIRE(SpectralFiniteStreamRule::make(256, 64, 16, 769));
}

TEST_CASE("A hop that does not divide the FFT breaks the dividing-hop closed form",
          "[spectral][eof]") {
    // This is why the dividing-hop policy refuses such hops:
    // ceil((L + N - H) / H) is not the number of grid frames overlapping the
    // source and N / H is not the drain. The analysis-grid count is.
    const std::uint64_t n = 256, h = 100, l = 1;
    REQUIRE((l + n - h + h - 1) / h == 2);
    REQUIRE(n / h == 2);
    REQUIRE(grid_frames(n, h, l) == 3);
    const auto rule = SpectralFiniteStreamRule::make(n, h, 1, l);
    REQUIRE(rule.frame_count == 3);
    REQUIRE(rule.drain_frames == 3);
}

TEST_CASE("Finite stream rule bounds length and capacity against signed chronology",
          "[spectral][eof]") {
    constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    for (const auto& g : geometries) {
        CAPTURE(g.fft, g.hop, g.channels);
        REQUIRE(SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, 0).status ==
                Status::invalid_length);
        const auto too_long = limit - (g.fft - 1) + 1;
        REQUIRE(SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, too_long).status ==
                Status::invalid_length);
        REQUIRE(SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, too_long - 1).status ==
                Status::capacity_exceeded);
        // The longest source keeps one window of headroom past its cutoff: its
        // last grid start is the largest hop multiple at most limit - 3N - H.
        const auto last_start = (limit - 3 * g.fft - g.hop) / g.hop * g.hop;
        const auto longest = last_start + g.hop;
        const auto rule = SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, longest);
        REQUIRE(rule);
        REQUIRE(rule.frame_count == last_start / g.hop + (g.fft - 1) / g.hop + 1);
        REQUIRE(rule.render_cutoff <= limit - g.fft);
        REQUIRE(rule.render_cutoff + g.fft > limit - g.hop);
        REQUIRE(SpectralFiniteStreamRule::make(g.fft, g.hop, g.channels, longest + 1).status ==
                Status::capacity_exceeded);
        if (g.fft % g.hop == 0) {
            // The dividing-hop bound this reproduces.
            const auto max_frames = (limit - (g.fft + g.hop) - g.fft) / g.hop;
            REQUIRE(rule.frame_count == max_frames);
            REQUIRE(longest == max_frames * g.hop - (g.fft - g.hop));
        }
    }
}
