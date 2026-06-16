// test_soft_bypass.cpp — click-free bypass wrapper over DSP processors.

#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/processor_chain.hpp>
#include <pulp/signal/soft_bypass.hpp>

#include <cmath>

using pulp::signal::ProcessorChain;
using pulp::signal::SoftBypass;

namespace {
// A trivial processor: scales by a gain. float process(float) + reset().
struct Gain {
    float g = 2.0f;
    float process(float x) { return x * g; }
    void reset() {}
};
struct AddOne {
    float process(float x) { return x + 1.0f; }
};
}  // namespace

TEST_CASE("SoftBypass passes the wet signal when active", "[signal][bypass]") {
    SoftBypass<Gain> sb;            // starts active
    REQUIRE_FALSE(sb.bypassed());
    REQUIRE(sb.wet_mix() == 1.0f);
    REQUIRE(sb.process(1.0f) == 2.0f);  // gain of 2, fully wet
    REQUIRE(sb.process(3.0f) == 6.0f);
}

TEST_CASE("SoftBypass settles to the dry signal when bypassed",
          "[signal][bypass]") {
    SoftBypass<Gain> sb;
    sb.set_fade_samples(16);
    sb.set_bypassed(true);
    REQUIRE(sb.bypassed());

    // Run past the fade; output must equal the dry input exactly.
    for (int i = 0; i < 64; ++i) sb.process(1.0f);
    REQUIRE_FALSE(sb.fading());
    REQUIRE(sb.wet_mix() == 0.0f);
    REQUIRE(sb.process(0.5f) == 0.5f);   // dry
    REQUIRE(sb.process(-2.0f) == -2.0f);
}

TEST_CASE("SoftBypass crossfade is monotonic and click-free", "[signal][bypass]") {
    SoftBypass<Gain> sb;  // gain 2
    sb.set_fade_samples(32);
    sb.set_bypassed(true);

    // With a constant input of 1.0, wet=2.0 and dry=1.0, so the output should
    // glide monotonically from 2.0 down to 1.0 with no jump larger than one
    // fade step (1/32 of the 1.0 span).
    float prev = sb.process(1.0f);
    float max_step = 0.0f;
    for (int i = 0; i < 80; ++i) {
        const float out = sb.process(1.0f);
        REQUIRE(out <= prev + 1e-6f);          // never increases (heading to dry)
        REQUIRE(out >= 1.0f - 1e-6f);          // never overshoots past dry
        max_step = std::max(max_step, std::abs(out - prev));
        prev = out;
    }
    REQUIRE(prev == 1.0f);                      // fully bypassed
    REQUIRE(max_step < 0.05f);                  // ~1/32 == 0.03125 per step
}

TEST_CASE("SoftBypass re-engage fades back to wet", "[signal][bypass]") {
    SoftBypass<Gain> sb;
    sb.set_fade_samples(16);
    sb.set_bypassed(true);
    for (int i = 0; i < 32; ++i) sb.process(1.0f);
    REQUIRE(sb.process(1.0f) == 1.0f);  // dry

    sb.set_bypassed(false);
    for (int i = 0; i < 32; ++i) sb.process(1.0f);
    REQUIRE_FALSE(sb.fading());
    REQUIRE(sb.process(1.0f) == 2.0f);  // back to full wet
}

TEST_CASE("SoftBypass reset settles the fade immediately", "[signal][bypass]") {
    SoftBypass<Gain> sb;
    sb.set_bypassed(true);
    sb.process(1.0f);
    REQUIRE(sb.fading());
    sb.reset();
    REQUIRE_FALSE(sb.fading());
    REQUIRE(sb.process(1.0f) == 1.0f);  // settled to dry
}

TEST_CASE("SoftBypass wraps a ProcessorChain", "[signal][bypass]") {
    // Chain: x -> (+1) -> (*2). active output for x=1 is (1+1)*2 = 4.
    SoftBypass<ProcessorChain<AddOne, Gain>> sb;
    REQUIRE(sb.process(1.0f) == 4.0f);

    sb.set_fade_samples(8);
    sb.set_bypassed(true);
    for (int i = 0; i < 32; ++i) sb.process(1.0f);
    REQUIRE(sb.process(1.0f) == 1.0f);  // dry passthrough of the whole chain
}
