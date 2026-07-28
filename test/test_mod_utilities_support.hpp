#pragma once

// Tier 0 mod-utilities toolkit — the shared modulation infrastructure the DSP
// series composes (see planning/2026-07-25-dsp-series-round2.md, adjudication
// A-1).
//
// The suite asserts the CONTRACTS other modules' specs cite by name — a
// triangle whose 0.5 phase offset is an exact inversion, an LFO rate accurate
// enough for the chorus spec's ±0.01 % zero-crossing test, a constant-time slew
// that arrives exactly, seeded randomness that is bit-reproducible. Expected
// values are computed from the shipped constants rather than restated, so a
// constant that changes fails the test that documents it instead of silently
// disagreeing with it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/envelope.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/lowpass_gate.hpp>
#include <pulp/signal/mod_matrix.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/slew_limiter.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/vactrol.hpp>
#include <pulp/signal/vca.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kSr = 48000.0;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

/// Measures a rendered signal's frequency from its upward zero crossings — the
/// measurement the chorus/flanger specs use to assert LFO rate accuracy end to
/// end.
///
/// Measures between the FIRST and LAST crossing rather than dividing a crossing
/// count by the render length. Counting over a fixed window is off by up to one
/// crossing depending on where the window boundaries fall relative to the
/// waveform — at 3 Hz over 100 s that is a 0.33 % measurement error, which
/// would swamp the 0.01 % accuracy being tested. Between two crossings the
/// span is exactly `count − 1` periods with no boundary term at all.
double measure_rate_hz(const std::vector<double>& x, double sample_rate) {
    std::size_t first = 0, last = 0;
    int count = 0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (x[i - 1] < 0.0 && x[i] >= 0.0) {
            if (count == 0) first = i;
            last = i;
            ++count;
        }
    }
    if (count < 2) return 0.0;
    const double periods = static_cast<double>(count - 1);
    const double span_samples = static_cast<double>(last - first);
    return periods * sample_rate / span_samples;
}

}  // namespace
