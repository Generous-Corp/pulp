#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/instrument_envelope.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>
#include <cmath>

using Catch::Matchers::WithinAbs;
using pulp::audio::AhdsrEnvelope;
using pulp::audio::AhdsrEnvelopeConfig;
using pulp::audio::EnvelopeStage;

TEST_CASE("AhdsrEnvelope renders attack hold decay sustain release stages",
          "[audio][sampler][envelope]") {
    AhdsrEnvelope envelope;
    REQUIRE(envelope.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 4.0,
        .attack_seconds = 0.5,
        .hold_seconds = 0.25,
        .decay_seconds = 0.5,
        .sustain_level = 0.5,
        .release_seconds = 0.5,
    }));

    envelope.note_on();
    REQUIRE(envelope.stage() == EnvelopeStage::Attack);
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.5f, 1.0e-6f));
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(envelope.stage() == EnvelopeStage::Hold);

    REQUIRE_THAT(envelope.next_sample(), WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(envelope.stage() == EnvelopeStage::Decay);
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.75f, 1.0e-6f));
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.5f, 1.0e-6f));
    REQUIRE(envelope.stage() == EnvelopeStage::Sustain);
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.5f, 1.0e-6f));

    envelope.note_off();
    REQUIRE(envelope.stage() == EnvelopeStage::Release);
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.25f, 1.0e-6f));
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.0f, 1.0e-6f));
    REQUIRE(envelope.stage() == EnvelopeStage::Idle);
    REQUIRE_FALSE(envelope.active());
}

TEST_CASE("AhdsrEnvelope supports ADSR via zero hold time",
          "[audio][sampler][envelope]") {
    AhdsrEnvelope envelope;
    REQUIRE(envelope.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 10.0,
        .attack_seconds = 0.0,
        .hold_seconds = 0.0,
        .decay_seconds = 0.2,
        .sustain_level = 0.25,
        .release_seconds = 0.0,
    }));

    envelope.note_on();
    REQUIRE(envelope.stage() == EnvelopeStage::Decay);
    REQUIRE_THAT(envelope.value(), WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.625f, 1.0e-6f));
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.25f, 1.0e-6f));
    REQUIRE(envelope.stage() == EnvelopeStage::Sustain);

    envelope.note_off();
    REQUIRE(envelope.stage() == EnvelopeStage::Idle);
    REQUIRE_THAT(envelope.value(), WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("AhdsrEnvelope preserves peak samples across zero-decay transitions",
          "[audio][sampler][envelope]") {
    AhdsrEnvelope attack_to_sustain;
    REQUIRE(attack_to_sustain.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 2.0,
        .attack_seconds = 1.0,
        .hold_seconds = 0.0,
        .decay_seconds = 0.0,
        .sustain_level = 0.25,
        .release_seconds = 0.0,
    }));
    attack_to_sustain.note_on();
    REQUIRE_THAT(attack_to_sustain.next_sample(), WithinAbs(0.5f, 1.0e-6f));
    REQUIRE_THAT(attack_to_sustain.next_sample(), WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(attack_to_sustain.stage() == EnvelopeStage::Sustain);
    REQUIRE_THAT(attack_to_sustain.next_sample(), WithinAbs(0.25f, 1.0e-6f));

    AhdsrEnvelope hold_to_sustain;
    REQUIRE(hold_to_sustain.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 2.0,
        .attack_seconds = 0.0,
        .hold_seconds = 0.5,
        .decay_seconds = 0.0,
        .sustain_level = 0.25,
        .release_seconds = 0.0,
    }));
    hold_to_sustain.note_on();
    REQUIRE(hold_to_sustain.stage() == EnvelopeStage::Hold);
    REQUIRE_THAT(hold_to_sustain.next_sample(), WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(hold_to_sustain.stage() == EnvelopeStage::Sustain);
    REQUIRE_THAT(hold_to_sustain.next_sample(), WithinAbs(0.25f, 1.0e-6f));

    AhdsrEnvelope attack_hold_to_sustain;
    REQUIRE(attack_hold_to_sustain.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 2.0,
        .attack_seconds = 0.5,
        .hold_seconds = 0.5,
        .decay_seconds = 0.0,
        .sustain_level = 0.25,
        .release_seconds = 0.0,
    }));
    attack_hold_to_sustain.note_on();
    REQUIRE_THAT(attack_hold_to_sustain.next_sample(), WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(attack_hold_to_sustain.stage() == EnvelopeStage::Hold);
    REQUIRE_THAT(attack_hold_to_sustain.next_sample(), WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(attack_hold_to_sustain.stage() == EnvelopeStage::Sustain);
    REQUIRE_THAT(attack_hold_to_sustain.next_sample(), WithinAbs(0.25f, 1.0e-6f));
}

TEST_CASE("AhdsrEnvelope releases from the current attack level",
          "[audio][sampler][envelope]") {
    AhdsrEnvelope envelope;
    REQUIRE(envelope.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 4.0,
        .attack_seconds = 1.0,
        .hold_seconds = 0.0,
        .decay_seconds = 0.0,
        .sustain_level = 1.0,
        .release_seconds = 0.5,
    }));

    envelope.note_on();
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.25f, 1.0e-6f));
    envelope.note_off();
    REQUIRE(envelope.stage() == EnvelopeStage::Release);
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.125f, 1.0e-6f));
    REQUIRE_THAT(envelope.next_sample(), WithinAbs(0.0f, 1.0e-6f));
    REQUIRE(envelope.stage() == EnvelopeStage::Idle);
}

TEST_CASE("AhdsrEnvelope rejects invalid configuration",
          "[audio][sampler][envelope]") {
    AhdsrEnvelope envelope;
    REQUIRE_FALSE(envelope.prepare(AhdsrEnvelopeConfig{.sample_rate = 0.0}));
    REQUIRE_FALSE(envelope.prepare(AhdsrEnvelopeConfig{.attack_seconds = -0.1}));
    REQUIRE_FALSE(envelope.prepare(AhdsrEnvelopeConfig{.sustain_level = 1.5}));
    REQUIRE_FALSE(envelope.prepare(AhdsrEnvelopeConfig{.release_seconds = std::nan("")}));
    REQUIRE_FALSE(envelope.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 1.0e308,
        .attack_seconds = 1.0e308,
    }));
}

TEST_CASE("AhdsrEnvelope renders blocks into caller-owned buffers",
          "[audio][sampler][envelope]") {
    AhdsrEnvelope envelope;
    REQUIRE(envelope.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 4.0,
        .attack_seconds = 0.5,
        .hold_seconds = 0.0,
        .decay_seconds = 0.0,
        .sustain_level = 1.0,
        .release_seconds = 0.5,
    }));

    std::array<float, 4> block{};
    envelope.note_on();
    envelope.render(block);
    REQUIRE_THAT(block[0], WithinAbs(0.5f, 1.0e-6f));
    REQUIRE_THAT(block[1], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(block[2], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(block[3], WithinAbs(1.0f, 1.0e-6f));
}

TEST_CASE("AhdsrEnvelope hot rendering does not allocate",
          "[audio][sampler][envelope][rt]") {
    AhdsrEnvelope envelope;
    REQUIRE(envelope.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 48000.0,
        .attack_seconds = 0.001,
        .hold_seconds = 0.0,
        .decay_seconds = 0.001,
        .sustain_level = 0.7,
        .release_seconds = 0.001,
    }));

    std::array<float, 64> block{};
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        envelope.note_on();
        envelope.render(block);
        envelope.note_off();
        envelope.render(block);
    }

    REQUIRE(block.back() >= 0.0f);
}
