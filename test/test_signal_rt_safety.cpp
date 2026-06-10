#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/oversampling.hpp>

#include <array>
#include <cstddef>

using pulp::signal::Oversampler;

namespace {

void require_process_allocates_no_memory(Oversampler::Kind kind,
                                         Oversampler::Factor factor) {
    Oversampler os;
    os.set_kind(kind);
    os.set_factor(factor);
    os.set_sample_rate(48000.0f);

    std::array<float, 16> inputs {};
    for (std::size_t i = 0; i < inputs.size(); ++i)
        inputs[i] = static_cast<float>(i + 1) * 0.01f;

    int callback_hits = 0;
    auto saturate = [&](float sample) {
        ++callback_hits;
        return sample / (1.0f + sample * sample);
    };

    pulp::test::RtAllocationProbe probe;
    for (float input : inputs) {
        const float output = os.process(input, saturate);
        (void)output;
    }

    REQUIRE(probe.allocation_count() == 0);
    REQUIRE(callback_hits == static_cast<int>(inputs.size()) * os.factor_value());
}

} // namespace

TEST_CASE("Oversampler process is allocation-free after configuration",
          "[signal][oversampling][rt-safety]") {
    require_process_allocates_no_memory(Oversampler::Kind::fir_biquad,
                                        Oversampler::Factor::x2);
    require_process_allocates_no_memory(Oversampler::Kind::fir_biquad,
                                        Oversampler::Factor::x4);
    require_process_allocates_no_memory(Oversampler::Kind::polyphase_iir,
                                        Oversampler::Factor::x2);
    require_process_allocates_no_memory(Oversampler::Kind::polyphase_iir,
                                        Oversampler::Factor::x4);
}

TEST_CASE("Oversampler process_block is allocation-free after configuration",
          "[signal][oversampling][rt-safety]") {
    Oversampler os;
    os.set_kind(Oversampler::Kind::polyphase_iir);
    os.set_factor(Oversampler::Factor::x4);
    os.set_sample_rate(48000.0f);

    std::array<float, 32> input {};
    std::array<float, 32> output {};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i % 7) * 0.05f;

    int callback_hits = 0;
    auto waveshape = [&](float sample) {
        ++callback_hits;
        return sample - (0.25f * sample * sample * sample);
    };

    pulp::test::RtAllocationProbe probe;
    os.process_block(input.data(), output.data(), input.size(), waveshape);

    REQUIRE(probe.allocation_count() == 0);
    REQUIRE(callback_hits == static_cast<int>(input.size()) * os.factor_value());
}
