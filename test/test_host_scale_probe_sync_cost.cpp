// What the per-block parameter sync costs at 2,000 parameters, measured
// through each format's real bundle (the Pulp Scale Probe fixture).
//
// A quiet block (no parameter written since the last one) is the cheap case:
// the adapters that sync per block skip their snapshot. A block after any
// write re-reads and re-compares every value, so one host-automated parameter
// per block is the worst steady case. Both are timed over 2,000 blocks of 64
// frames and printed, median and p99, and the automated median is held to a
// deliberately generous bound so the check survives a loaded or sanitized
// runner. Wall-clock timing: labelled `performance`, outside the required
// gate. The required gate holds the same path to zero allocations
// (test_host_scale_probe_bundles.cpp).

#include <catch2/catch_test_macros.hpp>

#include "scale_probe_hosting.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using namespace pulp::test::scale_probe;

namespace {

constexpr int kFrames = 64;
constexpr int kBlocks = 2000;
// About 1.3 ms of audio per block at 48 kHz; a sync that took most of it would
// be a real problem. The expected cost is a few microseconds.
constexpr double kAutomatedMedianBoundUs = 1000.0;

struct Spread {
    double median_us = 0.0;
    double p99_us = 0.0;
};

Spread spread_of(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return {samples[samples.size() / 2], samples[(samples.size() * 99) / 100]};
}

} // namespace

TEST_CASE("Per-block parameter sync at 2,000 parameters stays bounded in every format",
          "[host][scale][performance]") {
    using clock = std::chrono::steady_clock;
    for (const auto& target : targets()) {
        INFO(target.label);
        std::string why;
        auto slot = load(target, why);
        INFO(why);
        REQUIRE(slot);
        REQUIRE(slot->prepare(48000.0, kFrames));
        Block<kFrames> block;
        for (int i = 0; i < 64; ++i)
            block.run(*slot);

        std::vector<double> quiet;
        std::vector<double> automated;
        quiet.reserve(kBlocks);
        automated.reserve(kBlocks);
        for (int i = 0; i < kBlocks; ++i) {
            const auto start = clock::now();
            block.run(*slot);
            quiet.push_back(
                std::chrono::duration<double, std::micro>(clock::now() - start).count());
        }
        for (int i = 0; i < kBlocks; ++i) {
            const int index = 2 + (i * 7) % (Probe::kParamCount - 2);
            block.params.push({Probe::param_id(index), 0, (i % 2) ? 0.25f : 0.75f, 0});
            const auto start = clock::now();
            block.run(*slot);
            automated.push_back(
                std::chrono::duration<double, std::micro>(clock::now() - start).count());
        }
        const auto q = spread_of(quiet);
        const auto a = spread_of(automated);
        WARN(target.label << ": quiet block median " << q.median_us << " us, p99 " << q.p99_us
                          << " us; automated block median " << a.median_us << " us, p99 "
                          << a.p99_us << " us (2,000 parameters, 64 frames)");
        CHECK(a.median_us < kAutomatedMedianBoundUs);
    }
}
