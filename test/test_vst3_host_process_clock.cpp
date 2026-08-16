#include <catch2/catch_test_macros.hpp>

#include "plugin_slot_vst3_internal.hpp"

using pulp::host::Vst3HostProcessClock;
using Steinberg::Vst::ProcessContext;

TEST_CASE("VST3 host process clock publishes the prepared sample rate and a continuous timeline",
          "[host][vst3][process-context]") {
    Vst3HostProcessClock clock;
    clock.prepare(48000.0);

    ProcessContext first{};
    clock.write(first);
    REQUIRE(first.sampleRate == 48000.0);
    REQUIRE(first.projectTimeSamples == 0);
    REQUIRE((first.state & ProcessContext::kPlaying) != 0);

    clock.advance(512);
    ProcessContext second{};
    clock.write(second);
    REQUIRE(second.sampleRate == 48000.0);
    REQUIRE(second.projectTimeSamples == 512);

    clock.advance(127);
    ProcessContext third{};
    clock.write(third);
    REQUIRE(third.projectTimeSamples == 639);
}

TEST_CASE("VST3 host process clock resets at each prepare",
          "[host][vst3][process-context]") {
    Vst3HostProcessClock clock;
    clock.prepare(96000.0);
    clock.advance(2048);
    REQUIRE(clock.sample_position() == 2048);

    clock.prepare(44100.0);
    REQUIRE(clock.sample_rate() == 44100.0);
    REQUIRE(clock.sample_position() == 0);
}
