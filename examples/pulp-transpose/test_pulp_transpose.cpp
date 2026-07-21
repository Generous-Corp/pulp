// PulpTranspose MIDI-effect behavior, driven straight through the Processor —
// no format adapter, no host. What a transposer has to get right is which
// messages carry a note number and which must survive byte-identical.

#include <catch2/catch_test_macros.hpp>

#include "transpose_processor.hpp"

#include <cstdint>
#include <vector>

using pulp::examples::transpose::TransposeProcessor;

namespace {

struct Harness {
    Harness() {
        processor.set_state_store(&store);
        processor.define_parameters(store);
        processor.prepare({});
    }

    void set_shift(float semitones, float octaves) {
        store.set_value(TransposeProcessor::kSemitones, semitones);
        store.set_value(TransposeProcessor::kOctaves, octaves);
    }

    void run() {
        out.clear();
        out.clear_sysex();
        pulp::audio::BufferView<float> no_output;
        pulp::audio::BufferView<const float> no_input;
        processor.process(no_output, no_input, in, out, {});
    }

    std::vector<std::uint8_t> message(std::size_t index) const {
        const auto& ev = out[index];
        return {ev.message.data(), ev.message.data() + ev.message.length()};
    }

    pulp::state::StateStore store;
    TransposeProcessor processor;
    pulp::midi::MidiBuffer in;
    pulp::midi::MidiBuffer out;
};

pulp::midi::MidiEvent short_message(std::uint8_t status, std::uint8_t d1,
                                    std::uint8_t d2, std::int32_t offset = 0) {
    return {choc::midi::ShortMessage(status, d1, d2), offset, 0.0};
}

}  // namespace

TEST_CASE("PulpTranspose shifts notes by semitones plus octaves",
          "[example][transpose][midi]")
{
    Harness h;
    h.set_shift(3.0f, 1.0f);  // +15 semitones

    h.in.add(short_message(0x90, 60, 100));
    h.in.add(short_message(0x80, 60, 0, 32));
    h.run();

    REQUIRE(h.out.size() == 2);
    REQUIRE(h.message(0) == std::vector<std::uint8_t>{0x90, 75, 100});
    REQUIRE(h.message(1) == std::vector<std::uint8_t>{0x80, 75, 0});
    REQUIRE(h.out[1].sample_offset == 32);
}

TEST_CASE("PulpTranspose transposes poly aftertouch but not control changes",
          "[example][transpose][midi]")
{
    Harness h;
    h.set_shift(2.0f, 0.0f);

    h.in.add(short_message(0xA0, 60, 40));  // poly aftertouch: byte 1 is a note
    h.in.add(short_message(0xB0, 74, 90));  // CC 74 must NOT become CC 76
    h.in.add(short_message(0xE0, 0, 96));   // pitch bend passes through
    h.run();

    REQUIRE(h.out.size() == 3);
    REQUIRE(h.message(0) == std::vector<std::uint8_t>{0xA0, 62, 40});
    REQUIRE(h.message(1) == std::vector<std::uint8_t>{0xB0, 74, 90});
    REQUIRE(h.message(2) == std::vector<std::uint8_t>{0xE0, 0, 96});
}

TEST_CASE("PulpTranspose clamps notes that would leave the MIDI range",
          "[example][transpose][midi]")
{
    Harness h;

    h.set_shift(12.0f, 3.0f);  // +48
    h.in.add(short_message(0x90, 120, 100));
    h.run();
    REQUIRE(h.message(0) == std::vector<std::uint8_t>{0x90, 127, 100});

    h.in.clear();
    h.set_shift(-12.0f, -3.0f);  // -48
    h.in.add(short_message(0x90, 10, 100));
    h.run();
    REQUIRE(h.message(0) == std::vector<std::uint8_t>{0x90, 0, 100});
}

TEST_CASE("PulpTranspose passes SysEx through untouched",
          "[example][transpose][sysex]")
{
    Harness h;
    h.set_shift(7.0f, 0.0f);

    const std::vector<std::uint8_t> payload{0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
    h.in.add_sysex_copy(payload.data(), payload.size(), /*sample_offset=*/16);
    h.run();

    REQUIRE(h.out.size() == 0);
    REQUIRE(h.out.sysex_size() == 1);
    REQUIRE(h.out.sysex()[0].data == payload);
    REQUIRE(h.out.sysex()[0].sample_offset == 16);
}

TEST_CASE("PulpTranspose declares a MIDI-effect descriptor with no audio buses",
          "[example][transpose][descriptor]")
{
    Harness h;
    const auto desc = h.processor.descriptor();

    REQUIRE(desc.category == pulp::format::PluginCategory::MidiEffect);
    REQUIRE(desc.accepts_midi);
    REQUIRE(desc.produces_midi);
    REQUIRE(desc.input_buses.empty());
    REQUIRE(desc.output_buses.empty());
}
