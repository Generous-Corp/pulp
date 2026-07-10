// MF-3 denormal null test.
//
// Proves the newly-added `snap_to_zero` feedback-state writes are numerically
// transparent to real audio: rendering a continuously-excited signal through
// every touched recursive filter with the guard ON is BIT-IDENTICAL to
// rendering it with the guard compiled OUT (the pre-change reference).
//
// A second case feeds a decaying impulse into silence and asserts the guarded
// filters flush to a clean zero tail (no subnormals) — the property the guard
// exists to provide on FTZ-less targets (wasm, MSVC/ARM64, graph workers).

#include <catch2/catch_test_macros.hpp>

#include "denormal_null_reference.hpp"

#include <pulp/signal/denormal.hpp>

#include <vector>

namespace {

void require_bit_exact(const std::vector<float>& actual,
                       const std::vector<float>& reference) {
    REQUIRE(actual.size() == reference.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        // Bit-for-bit equality (not an epsilon): the snap threshold is never
        // reached by this excitation, so the guard must be a strict no-op.
        REQUIRE(actual[i] == reference[i]);
    }
}

}  // namespace

TEST_CASE("MF-3: snap_to_zero is bit-exact vs snap-disabled on real audio",
          "[signal][denormal][null]") {
    // This TU is built with the shipping default (snap enabled).
    static_assert(PULP_DSP_ENABLE_SNAP_TO_ZERO == 1,
                  "test_denormal_null must be compiled with snap enabled");

    const denormal_null::AllOutputs snap_on = denormal_null::render_all();
    const denormal_null::AllOutputs snap_off = denormal_null_reference();

    require_bit_exact(snap_on.biquad, snap_off.biquad);
    require_bit_exact(snap_on.svf, snap_off.svf);
    require_bit_exact(snap_on.ladder, snap_off.ladder);
    require_bit_exact(snap_on.ballistics, snap_off.ballistics);
    require_bit_exact(snap_on.dc_blocker, snap_off.dc_blocker);
    require_bit_exact(snap_on.tpt, snap_off.tpt);
    require_bit_exact(snap_on.reverb, snap_off.reverb);
    require_bit_exact(snap_on.phaser, snap_off.phaser);
    require_bit_exact(snap_on.noise_gate, snap_off.noise_gate);
    require_bit_exact(snap_on.compressor, snap_off.compressor);
    require_bit_exact(snap_on.limiter, snap_off.limiter);
}

TEST_CASE("MF-3: guarded feedback filters flush to a subnormal-free tail",
          "[signal][denormal]") {
    // A/B on a decaying-into-silence tail:
    //   * the snap-OFF reference build MUST hit the subnormal range for at
    //     least one filter — proof the configs actually decay that far, so the
    //     snap-ON result below is not vacuously clean;
    //   * the snap-ON build (this TU) must produce NO subnormal tail sample.
    const denormal_null::TailReport snap_off = denormal_null_reference_tail();
    const denormal_null::TailReport snap_on = denormal_null::render_tails();

    // The guard flushes every feedback state cleanly.
    REQUIRE_FALSE(snap_on.dc_blocker);
    REQUIRE_FALSE(snap_on.ballistics);
    REQUIRE_FALSE(snap_on.svf);
    REQUIRE_FALSE(snap_on.ladder);
    REQUIRE_FALSE(snap_on.reverb);

    // The test has teeth: without the guard, these tails go subnormal.
    const bool reference_saw_subnormals =
        snap_off.dc_blocker || snap_off.ballistics || snap_off.svf ||
        snap_off.ladder || snap_off.reverb;
    REQUIRE(reference_saw_subnormals);
}
