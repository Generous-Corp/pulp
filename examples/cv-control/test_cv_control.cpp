#include "cv_control.hpp"
#include "cv_control_processor.hpp"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>
#include <pulp/runtime/triple_buffer.hpp>
#include <thread>

using pulp::examples::cv_control::ControlReplay;
using pulp::examples::cv_control::ControlSample;
using pulp::examples::cv_control::decode_midi_control;
using pulp::examples::cv_control::FaceFrame;
using pulp::examples::cv_control::FaceMapping;
using pulp::examples::cv_control::HandFrame;
using pulp::examples::cv_control::HandMapping;
using pulp::examples::cv_control::HandReplay;

struct ProcessorFixture {
    pulp::state::StateStore store;
    pulp::examples::cv_control::FaceToneProcessor processor;

    ProcessorFixture() {
        processor.set_state_store(&store);
        processor.define_parameters(store);
        pulp::format::PrepareContext context{};
        context.input_channels = 0;
        processor.prepare(context);
    }
    ~ProcessorFixture() {
        processor.release();
    }

    void render(pulp::midi::MidiBuffer& input, int samples = 512) {
        pulp::audio::Buffer<float> output(2, samples);
        auto view = output.view();
        pulp::audio::BufferView<const float> empty;
        pulp::midi::MidiBuffer midi_out;
        pulp::format::ProcessContext context;
        context.sample_rate = 48000.0;
        context.num_samples = samples;
        processor.process(view, empty, input, midi_out, context);
    }
};

TEST_CASE("processor combines host and face modulation without changing automation base") {
    ProcessorFixture fixture;
    fixture.store.set_value_rt(pulp::examples::cv_control::kBrightness, 0.2f);
    fixture.store.set_mod_offset(pulp::examples::cv_control::kBrightness, 0.1f);
    fixture.processor.submit_face_frame({1, 1.0f, 1.0f, true});
    pulp::midi::MidiBuffer input;
    fixture.render(input);
    REQUIRE_THAT(fixture.store.get_value(pulp::examples::cv_control::kBrightness),
                 Catch::Matchers::WithinAbs(0.2f, 1e-6f));
    REQUIRE_THAT(*fixture.store.displayed_modulation(pulp::examples::cv_control::kBrightness),
                 Catch::Matchers::WithinAbs(0.425f, 1e-6f));
    fixture.render(input, 11999);
    REQUIRE_THAT(*fixture.store.displayed_modulation(pulp::examples::cv_control::kBrightness),
                 Catch::Matchers::WithinAbs(0.425f, 1e-6f));
    fixture.render(input, 1); // 250 ms of audio since the last provider frame
    REQUIRE_THAT(*fixture.store.displayed_modulation(pulp::examples::cv_control::kBrightness),
                 Catch::Matchers::WithinAbs(0.3f, 1e-6f));
    REQUIRE_THAT(fixture.store.get_value(pulp::examples::cv_control::kBrightness),
                 Catch::Matchers::WithinAbs(0.2f, 1e-6f));

    input.add(pulp::midi::MidiEvent::pitch_bend(15, 0x3fff));
    fixture.render(input);
    REQUIRE_THAT(*fixture.store.displayed_modulation(pulp::examples::cv_control::kBrightness),
                 Catch::Matchers::WithinAbs(0.425f, 1e-6f));
}

TEST_CASE("face mapping is coherent, smoothed, and confidence gated") {
    FaceMapping mapping(0.6f, 0.5f);
    mapping.submit({1, 1.0f, 0.9f, true});
    REQUIRE_THAT(mapping.value(), Catch::Matchers::WithinAbs(0.5f, 1e-6f));
    REQUIRE(mapping.valid());
    mapping.submit({2, 0.0f, 0.2f, true});
    REQUIRE_THAT(mapping.value(), Catch::Matchers::WithinAbs(0.25f, 1e-6f));
    REQUIRE_FALSE(mapping.valid());
}

TEST_CASE("tracking loss and stale replay expire at the exact audio sample") {
    FaceMapping mapping;
    mapping.submit({7, 0.8f, 1.0f, true});
    mapping.advance(11999, 12000);
    REQUIRE(mapping.valid());
    mapping.advance(1, 12000);
    REQUIRE(mapping.value() == 0.0f);
    REQUIRE_FALSE(mapping.valid());
    mapping.submit({1, 0.4f, 1.0f, true}); // provider may restart its sequence
    REQUIRE(mapping.valid());
    mapping.advance(12000, 12000);
    REQUIRE_FALSE(mapping.valid());
}

TEST_CASE("malformed values fail closed") {
    FaceMapping mapping;
    mapping.submit({1, std::numeric_limits<float>::quiet_NaN(), 1.0f, true});
    REQUIRE(mapping.value() == 0.0f);
    mapping.submit({2, 3.0f, 1.0f, true});
    REQUIRE_THAT(mapping.value(), Catch::Matchers::WithinAbs(0.25f, 1e-6f));
}

TEST_CASE("provider restart replaces an expired frame") {
    FaceMapping mapping;
    mapping.submit({10, 0.9f, 1.0f, true});
    mapping.expire();
    mapping.submit({11, 0.4f, 1.0f, true});
    REQUIRE(mapping.valid());
    REQUIRE(mapping.sequence() == 11);
}

TEST_CASE("native producer and audio reader exchange coherent samples") {
    pulp::runtime::TripleBuffer<ControlSample> samples;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= 10000; ++i)
            samples.write({i, static_cast<float>(i % 100) / 99.0f, 1.0f, true});
        done.store(true, std::memory_order_release);
    });
    FaceMapping mapping;
    std::uint64_t previous = 0;
    do {
        const auto frame = samples.read();
        if (frame.sequence != previous) {
            REQUIRE(frame.sequence > previous);
            REQUIRE_THAT(frame.value, Catch::Matchers::WithinAbs(
                                          static_cast<float>(frame.sequence % 100) / 99.0f, 1e-6f));
            mapping.submit(frame);
            previous = frame.sequence;
        }
    } while (!done.load(std::memory_order_acquire) || previous != 10000);
    producer.join();
    REQUIRE(std::isfinite(mapping.value()));
    REQUIRE(mapping.value() >= 0.0f);
    REQUIRE(mapping.value() <= 1.0f);
}

TEST_CASE("short MIDI control packet validates status, bytes, and loss") {
    const auto full = decode_midi_control(0xEF, 0x7F, 0x7F, 1);
    REQUIRE(full.has_value());
    REQUIRE(full->valid);
    REQUIRE_THAT(full->value, Catch::Matchers::WithinAbs(1.0f, 1e-6f));
    const auto lost = decode_midi_control(0xEF, 0, 0, 2);
    REQUIRE(lost.has_value());
    REQUIRE_FALSE(lost->valid);
    REQUIRE_FALSE(decode_midi_control(0xEE, 1, 1, 3).has_value());
    REQUIRE_FALSE(decode_midi_control(0xEF, 128, 1, 3).has_value());
    REQUIRE_FALSE(decode_midi_control(0xEF, 1, 128, 3).has_value());
}

TEST_CASE("motion or sensor samples use the same mapping") {
    FaceMapping mapping;
    mapping.submit({42, 0.75f, 1.0f, true}); // e.g. normalized wrist rotation
    REQUIRE(mapping.valid());
    REQUIRE(mapping.sequence() == 42);
}

TEST_CASE("camera-free replay feeds the same mapping") {
    const ControlSample frames[] = {{1, .2f, 1.0f, true}, {2, .8f, 1.0f, true}};
    ControlReplay replay(frames);
    FaceMapping mapping;
    REQUIRE(replay.next(mapping));
    REQUIRE(replay.next(mapping));
    REQUIRE_FALSE(replay.next(mapping));
    REQUIRE(replay.position() == 2);
    replay.reset();
    REQUIRE(replay.position() == 0);
}

TEST_CASE("coherent multi-signal frames preserve one inference sequence") {
    HandMapping mapping(0.6f, 1.0f);
    mapping.submit({17, 0.2f, 0.4f, 0.8f, 0.9f, true});
    REQUIRE(mapping.x().sequence() == 17);
    REQUIRE(mapping.y().sequence() == 17);
    REQUIRE(mapping.pinch().sequence() == 17);
    REQUIRE(mapping.x().valid());
    REQUIRE(mapping.y().valid());
    REQUIRE(mapping.pinch().valid());
    REQUIRE_THAT(mapping.x().value(), Catch::Matchers::WithinAbs(0.2f, 1e-6f));
    REQUIRE_THAT(mapping.y().value(), Catch::Matchers::WithinAbs(0.4f, 1e-6f));
    REQUIRE_THAT(mapping.pinch().value(), Catch::Matchers::WithinAbs(0.8f, 1e-6f));
}

TEST_CASE("coherent multi-signal replay expires all related controls together") {
    const HandFrame frames[] = {{4, 0.1f, 0.2f, 0.3f, 1.0f, true},
                                {5, 0.7f, 0.8f, 0.9f, 1.0f, true}};
    HandReplay replay(frames);
    HandMapping mapping(0.6f, 1.0f);
    REQUIRE(replay.next(mapping));
    REQUIRE(replay.next(mapping));
    REQUIRE_FALSE(replay.next(mapping));
    REQUIRE(mapping.x().sequence() == 5);
    REQUIRE(mapping.y().sequence() == 5);
    REQUIRE(mapping.pinch().sequence() == 5);
    mapping.advance(12000, 12000);
    REQUIRE_FALSE(mapping.x().valid());
    REQUIRE_FALSE(mapping.y().valid());
    REQUIRE_FALSE(mapping.pinch().valid());
    REQUIRE(mapping.x().value() == 0.0f);
    REQUIRE(mapping.y().value() == 0.0f);
    REQUIRE(mapping.pinch().value() == 0.0f);
}
