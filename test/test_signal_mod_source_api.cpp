// Public API compatibility contracts for modulation sources.
#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/lfo.hpp>

#include <type_traits>
#include <utility>

using namespace pulp::signal;

TEST_CASE("Lfo64 keeps the toolkit API and defaults without a waveform setter",
          "[signal][mod][lfo][compatibility]") {
    static_assert(std::is_same_v<decltype(std::declval<const Lfo64&>().wave()),
                                 Lfo64::Wave>);

    Lfo64 lfo;
    lfo.prepare(48000.0);
    lfo.set_rate_hz(1000.0);
    REQUIRE(lfo.rate_hz() == 1000.0);

    lfo.set_stereo_offset(0.25);
    double left = 0.0;
    double right = 0.0;
    lfo.next_stereo(left, right);
    REQUIRE(left != right);

    // The shipped enum-returning accessor remains usable in enum contexts.
    switch (lfo.wave()) {
        case Lfo64::Wave::sine: break;
        default: FAIL("default LFO waveform changed");
    }
}
