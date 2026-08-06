#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/utility_kernels.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace {

using namespace pulp;

struct NoteBalance {
    std::array<int, 16 * 128> depth{};
    bool valid = true;

    void feed(const midi::MidiBuffer& events) {
        for (const auto& event : events) {
            if (event.is_note_on()) {
                ++depth[event.channel() * 128 + event.note()];
            } else if (event.is_note_off()) {
                auto& value = depth[event.channel() * 128 + event.note()];
                if (value == 0)
                    valid = false;
                else
                    --value;
            }
        }
    }

    bool balanced() const {
        if (!valid)
            return false;
        for (const int value : depth)
            if (value != 0)
                return false;
        return true;
    }
};

midi::MidiBuffer prepared_buffer(std::size_t capacity = 64) {
    midi::MidiBuffer buffer;
    buffer.reserve(capacity);
    buffer.set_realtime_capacity_limit(true);
    return buffer;
}

float square_curve(float value, float) noexcept {
    return value * value;
}

float nan_curve(float, float) noexcept {
    return std::numeric_limits<float>::quiet_NaN();
}

void prepare_sidecars(midi::MidiBuffer& buffer, midi::UmpBuffer& ump) {
    buffer.reserve(64, 2, 8);
    buffer.set_realtime_capacity_limit(true);
    ump.reserve(2);
    ump.set_realtime_capacity_limit(true);
    buffer.attach_ump(&ump);
}

void seed_input_sidecars(midi::MidiBuffer& buffer, midi::UmpBuffer& ump) {
    const std::array<std::uint8_t, 4> payload{0xf0, 0x7d, 0x01, 0xf7};
    REQUIRE(buffer.add_sysex_copy(payload.data(), payload.size(), 9, 0.25));
    REQUIRE(ump.add(midi::UmpPacket::note_on_2(2, 3, 67, 0xbeef), 11));
}

void seed_stale_sidecars(midi::MidiBuffer& buffer, midi::UmpBuffer& ump) {
    const std::array<std::uint8_t, 2> stale{0xf0, 0xf7};
    REQUIRE(buffer.add_sysex_copy(stale.data(), stale.size(), 1));
    REQUIRE(ump.add(midi::UmpPacket::note_off_2(0, 0, 1), 1));
}

midi::MidiEvent poly_pressure(std::uint8_t channel, std::uint8_t note, std::uint8_t value) {
    return {
        choc::midi::ShortMessage(static_cast<std::uint8_t>(0xa0 | (channel & 0x0f)), note, value),
        0, 0.0};
}

midi::UmpPacket midi1_voice(std::uint8_t status, std::uint8_t channel, std::uint8_t data1,
                            std::uint8_t data2, std::uint8_t group = 0) {
    midi::UmpPacket packet;
    packet.word_count = 1;
    packet.words[0] = (0x2u << 28) | (static_cast<std::uint32_t>(group & 0x0f) << 24) |
                      (static_cast<std::uint32_t>(status | (channel & 0x0f)) << 16) |
                      (static_cast<std::uint32_t>(data1) << 8) | data2;
    return packet;
}

midi::UmpPacket midi2_voice(std::uint8_t status, std::uint8_t channel, std::uint8_t data1,
                            std::uint8_t data2, std::uint32_t value) {
    midi::UmpPacket packet;
    packet.word_count = 2;
    packet.words[0] = (0x4u << 28) | (static_cast<std::uint32_t>(status | (channel & 0x0f)) << 16) |
                      (static_cast<std::uint32_t>(data1) << 8) | data2;
    packet.words[1] = value;
    return packet;
}

void check_exact_sidecars(const midi::MidiBuffer& buffer) {
    REQUIRE(buffer.sysex().size() == 1);
    CHECK(buffer.sysex()[0].data == std::vector<std::uint8_t>{0xf0, 0x7d, 0x01, 0xf7});
    CHECK(buffer.sysex()[0].sample_offset == 9);
    REQUIRE(buffer.ump() != nullptr);
    REQUIRE(buffer.ump()->size() == 1);
    CHECK((*buffer.ump())[0].packet.words == midi::UmpPacket::note_on_2(2, 3, 67, 0xbeef).words);
    CHECK((*buffer.ump())[0].sample_offset == 11);
}

} // namespace

TEST_CASE("Note balance oracle rejects unmatched and transient early releases",
          "[midi][utility][test-oracle]") {
    auto events = prepared_buffer();
    events.add(midi::MidiEvent::note_off(0, 60));
    events.add(midi::MidiEvent::note_on(0, 60, 100));
    NoteBalance balance;
    balance.feed(events);
    CHECK_FALSE(balance.balanced());
}

TEST_CASE("MIDI channel routing, note ranges, and keyboard splits preserve protocol ranges",
          "[midi][utility][routing]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    input.add(midi::MidiEvent::note_on(3, 59, 100));
    input.add(midi::MidiEvent::note_off(3, 59));

    midi::ChannelRouteSpec route;
    route.accepted_channels = std::uint16_t{1} << 3;
    route.output_channel[3] = 15;
    midi::ChannelRouter router(route);
    REQUIRE(router.process(input, output).complete);
    REQUIRE(output.size() == 2);
    CHECK(output[0].channel() == 15);
    CHECK(output[0].note() == 59);

    midi::NoteRangeFilter range({60, 72});
    REQUIRE(range.process(input, output).complete);
    CHECK(output.empty());

    auto lower = prepared_buffer();
    auto upper = prepared_buffer();
    midi::KeyboardSplit split({60, true, 2, 14});
    input.clear();
    input.add(midi::MidiEvent::note_on(0, 59, 127));
    input.add(midi::MidiEvent::note_on(0, 60, 127));
    REQUIRE(split.process(input, lower, upper).complete);
    REQUIRE(lower.size() == 1);
    REQUIRE(upper.size() == 1);
    CHECK(lower[0].channel() == 2);
    CHECK(upper[0].channel() == 14);

    SECTION("invalid channel, range, and split specs are rejected") {
        auto invalid_route = route;
        invalid_route.output_channel[3] = 16;
        CHECK_FALSE(router.replace_spec(invalid_route));
        CHECK_FALSE(range.replace_spec({80, 40}));
        CHECK_FALSE(split.replace_spec({128, true, 0, 1}));
        midi::KeyboardSplit invalid_split({60, true, 0, 16});
        CHECK_FALSE(invalid_split.valid());
        CHECK_FALSE(invalid_split.process(input, lower, upper).complete);
    }
}

TEST_CASE("routing spec replacement cannot strand held notes",
          "[midi][utility][routing][lifecycle][ownership]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();

    SECTION("channel routes keep the accepting mapping through release") {
        midi::ChannelRouteSpec initial;
        initial.accepted_channels = std::uint16_t{1} << 3;
        initial.output_channel[3] = 15;
        midi::ChannelRouter router(initial);
        input.add(midi::MidiEvent::note_on(3, 64, 100));
        REQUIRE(router.process(input, output).complete);

        auto replacement = initial;
        replacement.output_channel[3] = 4;
        CHECK_FALSE(router.replace_spec(replacement));
        router.reset();
        CHECK_FALSE(router.replace_spec(replacement));
        input.clear();
        input.add(midi::MidiEvent::note_off(3, 64));
        REQUIRE(router.process(input, output).complete);
        REQUIRE(output.size() == 1);
        CHECK(output[0].channel() == 15);
        REQUIRE(router.replace_spec(replacement));
        input.clear();
        input.add(midi::MidiEvent::note_on(3, 65, 100));
        REQUIRE(router.process(input, output).complete);
        CHECK(output[0].channel() == 4);
    }

    SECTION("note ranges reject replacement until the accepted note releases") {
        midi::NoteRangeFilter range({60, 72});
        input.add(midi::MidiEvent::note_on(0, 64, 100));
        REQUIRE(range.process(input, output).complete);
        CHECK_FALSE(range.replace_spec({65, 72}));
        input.clear();
        input.add(midi::MidiEvent::note_off(0, 64));
        REQUIRE(range.process(input, output).complete);
        REQUIRE(output.size() == 1);
        CHECK(output[0].is_note_off());
        REQUIRE(range.replace_spec({65, 72}));
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 64, 100));
        REQUIRE(range.process(input, output).complete);
        CHECK(output.empty());
    }

    SECTION("keyboard splits keep the accepting side and channel through release") {
        midi::KeyboardSplit split({60, true, 2, 14});
        auto lower = prepared_buffer();
        auto upper = prepared_buffer();
        input.add(midi::MidiEvent::note_on(0, 59, 100));
        REQUIRE(split.process(input, lower, upper).complete);
        CHECK_FALSE(split.replace_spec({58, true, 5, 9}));
        input.clear();
        input.add(midi::MidiEvent::note_off(0, 59));
        REQUIRE(split.process(input, lower, upper).complete);
        REQUIRE(lower.size() == 1);
        CHECK(lower[0].channel() == 2);
        REQUIRE(split.replace_spec({58, true, 5, 9}));
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 57, 100));
        REQUIRE(split.process(input, lower, upper).complete);
        CHECK(lower[0].channel() == 5);
    }

    SECTION("failed MIDI releases are retained and drained before replacement") {
        midi::ChannelRouteSpec initial;
        initial.accepted_channels = std::uint16_t{1} << 3;
        initial.output_channel[3] = 15;
        midi::ChannelRouter router(initial);
        auto constrained = prepared_buffer(1);
        input.add(midi::MidiEvent::note_on(3, 64, 100));
        REQUIRE(router.process(input, constrained).complete);

        input.clear();
        input.add(midi::MidiEvent::cc(3, 7, 100));
        input.add(midi::MidiEvent::note_off(3, 64));
        CHECK_FALSE(router.process(input, constrained).complete);
        auto replacement = initial;
        replacement.output_channel[3] = 4;
        router.reset();
        CHECK_FALSE(router.replace_spec(replacement));

        input.clear();
        REQUIRE(router.process(input, constrained).complete);
        REQUIRE(constrained.size() == 1);
        CHECK(constrained[0].is_note_off());
        CHECK(constrained[0].channel() == 15);
        REQUIRE(router.replace_spec(replacement));
    }

    SECTION("releases for suppressed attacks cannot retire newer routed notes") {
        midi::ChannelRouteSpec initial;
        initial.accepted_channels = std::uint16_t{1} << 3;
        initial.output_channel[3] = 15;
        midi::ChannelRouter router(initial);
        auto constrained = prepared_buffer(1);
        auto replacement = initial;
        replacement.output_channel[3] = 4;

        input.add(midi::MidiEvent::cc(3, 7, 100));
        input.add(midi::MidiEvent::note_on(3, 64, 100));
        CHECK_FALSE(router.process(input, constrained).complete);
        CHECK_FALSE(router.replace_spec(replacement));

        input.clear();
        input.add(midi::MidiEvent::note_on(3, 64, 100));
        REQUIRE(router.process(input, constrained).complete);
        REQUIRE(constrained.size() == 1);
        CHECK(constrained[0].is_note_on());

        input.clear();
        input.add(midi::MidiEvent::note_off(3, 64));
        REQUIRE(router.process(input, constrained).complete);
        CHECK(constrained.empty());
        CHECK_FALSE(router.replace_spec(replacement));

        REQUIRE(router.process(input, constrained).complete);
        REQUIRE(constrained.size() == 1);
        CHECK(constrained[0].is_note_off());
        CHECK(constrained[0].channel() == 15);
        REQUIRE(router.replace_spec(replacement));
    }

    SECTION("range and split kernels retain failed releases") {
        {
            midi::NoteRangeFilter range({60, 72});
            auto constrained = prepared_buffer(1);
            input.add(midi::MidiEvent::note_on(0, 64, 100));
            REQUIRE(range.process(input, constrained).complete);
            input.clear();
            input.add(midi::MidiEvent::cc(0, 7, 100));
            input.add(midi::MidiEvent::note_off(0, 64));
            CHECK_FALSE(range.process(input, constrained).complete);
            CHECK_FALSE(range.replace_spec({65, 72}));
            input.clear();
            REQUIRE(range.process(input, constrained).complete);
            REQUIRE(constrained.size() == 1);
            CHECK(constrained[0].is_note_off());
            REQUIRE(range.replace_spec({65, 72}));
        }
        {
            midi::KeyboardSplit split({60, true, 2, 14});
            auto lower = prepared_buffer(1);
            auto upper = prepared_buffer(1);
            input.clear();
            input.add(midi::MidiEvent::note_on(0, 59, 100));
            REQUIRE(split.process(input, lower, upper).complete);
            input.clear();
            input.add(midi::MidiEvent::cc(0, 7, 100));
            input.add(midi::MidiEvent::note_off(0, 59));
            CHECK_FALSE(split.process(input, lower, upper).complete);
            CHECK_FALSE(split.replace_spec({58, true, 5, 9}));
            input.clear();
            REQUIRE(split.process(input, lower, upper).complete);
            REQUIRE(lower.size() == 1);
            CHECK(lower[0].is_note_off());
            CHECK(lower[0].channel() == 2);
            REQUIRE(split.replace_spec({58, true, 5, 9}));
        }
    }

    SECTION("failed grouped UMP releases are retained") {
        midi::ChannelRouteSpec initial;
        initial.accepted_channels = std::uint16_t{1} << 3;
        initial.output_channel[3] = 15;
        midi::ChannelRouter router(initial);
        midi::UmpBuffer input_ump;
        midi::UmpBuffer output_ump;
        input_ump.reserve(129);
        input_ump.set_realtime_capacity_limit(true);
        output_ump.set_realtime_capacity_limit(true);
        input.attach_ump(&input_ump);
        output.attach_ump(&output_ump);
        REQUIRE(input_ump.add(midi::UmpPacket::note_on_2(5, 3, 64, 0x8000), 0));
        REQUIRE(router.process(input, output).complete);

        input_ump.clear();
        for (std::size_t i = 0; i < output_ump.capacity(); ++i)
            REQUIRE(input_ump.add(midi::UmpPacket::cc_2(
                5, 3, 7, static_cast<std::uint32_t>(i)), 0));
        REQUIRE(input_ump.add(midi::UmpPacket::note_off_2(5, 3, 64), 1));
        CHECK_FALSE(router.process(input, output).complete);
        input_ump.clear();
        REQUIRE(router.process(input, output).complete);
        REQUIRE(output_ump.size() == 1);
        CHECK(output_ump[0].packet.message_type() == midi::UmpMessageType::Midi2ChannelVoice);
        CHECK(output_ump[0].packet.group() == 5);
        CHECK(output_ump[0].packet.channel() == 15);
        CHECK((output_ump[0].packet.status() & 0xf0) == 0x80);
    }

    SECTION("failed MIDI 1 UMP releases retain their protocol and group") {
        midi::ChannelRouteSpec initial;
        initial.accepted_channels = std::uint16_t{1} << 3;
        initial.output_channel[3] = 15;
        midi::ChannelRouter router(initial);
        midi::UmpBuffer input_ump;
        midi::UmpBuffer output_ump;
        input_ump.reserve(129);
        input_ump.set_realtime_capacity_limit(true);
        output_ump.set_realtime_capacity_limit(true);
        input.attach_ump(&input_ump);
        output.attach_ump(&output_ump);
        REQUIRE(input_ump.add(midi1_voice(0x90, 3, 64, 100, 6), 0));
        REQUIRE(router.process(input, output).complete);

        input_ump.clear();
        for (std::size_t i = 0; i < output_ump.capacity(); ++i)
            REQUIRE(input_ump.add(
                midi1_voice(0xb0, 3, 7, static_cast<std::uint8_t>(i & 0x7f), 6), 0));
        REQUIRE(input_ump.add(midi1_voice(0x80, 3, 64, 0, 6), 1));
        CHECK_FALSE(router.process(input, output).complete);
        input_ump.clear();
        REQUIRE(router.process(input, output).complete);
        REQUIRE(output_ump.size() == 1);
        CHECK(output_ump[0].packet.message_type() == midi::UmpMessageType::Midi1ChannelVoice);
        CHECK(output_ump[0].packet.group() == 6);
        CHECK(output_ump[0].packet.channel() == 15);
        CHECK((output_ump[0].packet.status() & 0xf0) == 0x80);
    }

    SECTION("UMP releases for suppressed attacks cannot retire newer notes") {
        midi::ChannelRouteSpec initial;
        initial.accepted_channels = std::uint16_t{1} << 3;
        initial.output_channel[3] = 15;
        midi::ChannelRouter router(initial);
        midi::UmpBuffer input_ump;
        midi::UmpBuffer output_ump;
        output_ump.set_realtime_capacity_limit(true);
        input_ump.reserve(output_ump.capacity() + 1);
        input.attach_ump(&input_ump);
        output.attach_ump(&output_ump);

        for (std::size_t i = 0; i < output_ump.capacity(); ++i)
            REQUIRE(input_ump.add(midi::UmpPacket::cc_2(
                5, 3, 7, static_cast<std::uint32_t>(i)), 0));
        REQUIRE(input_ump.add(midi::UmpPacket::note_on_2(5, 3, 64, 0x8000), 1));
        CHECK_FALSE(router.process(input, output).complete);

        input_ump.clear();
        REQUIRE(input_ump.add(midi::UmpPacket::note_on_2(5, 3, 64, 0x8000), 0));
        REQUIRE(router.process(input, output).complete);
        REQUIRE(output_ump.size() == 1);
        CHECK((output_ump[0].packet.status() & 0xf0) == 0x90);

        input_ump.clear();
        REQUIRE(input_ump.add(midi::UmpPacket::note_off_2(5, 3, 64), 0));
        REQUIRE(router.process(input, output).complete);
        CHECK(output_ump.empty());

        REQUIRE(router.process(input, output).complete);
        REQUIRE(output_ump.size() == 1);
        CHECK((output_ump[0].packet.status() & 0xf0) == 0x80);
        CHECK(output_ump[0].packet.group() == 5);
        CHECK(output_ump[0].packet.channel() == 15);
    }
}

TEST_CASE("MIDI utilities clear stale sidecars and preserve exact SysEx and UMP routing",
          "[midi][utility][sidecars]") {
    midi::MidiBuffer input;
    midi::UmpBuffer input_ump;
    prepare_sidecars(input, input_ump);
    input.add(midi::MidiEvent::note_on(0, 60, 100));
    seed_input_sidecars(input, input_ump);

    auto run_one_output = [&](auto& kernel, auto&& invoke) {
        midi::MidiBuffer destination;
        midi::UmpBuffer destination_ump;
        prepare_sidecars(destination, destination_ump);
        seed_stale_sidecars(destination, destination_ump);
        REQUIRE(invoke(kernel, destination).complete);
        check_exact_sidecars(destination);
    };

    midi::ChannelRouter router;
    run_one_output(router, [&](auto& kernel, auto& destination) {
        return kernel.process(input, destination);
    });
    midi::NoteRangeFilter filter;
    run_one_output(filter, [&](auto& kernel, auto& destination) {
        return kernel.process(input, destination);
    });
    midi::NoteLengthShaper<2> shaper({64});
    run_one_output(shaper, [&](auto& kernel, auto& destination) {
        return kernel.process(input, destination, timebase::SamplePosition{0}, 32);
    });
    midi::MonophonicNoteSelector selector;
    run_one_output(selector, [&](auto& kernel, auto& destination) {
        return kernel.process(input, destination);
    });
    midi::ControllerMapper<2> mapper;
    run_one_output(mapper, [&](auto& kernel, auto& destination) {
        return kernel.process(input, destination, 48000.0);
    });

    midi::MidiBuffer lower;
    midi::MidiBuffer upper;
    midi::UmpBuffer lower_ump;
    midi::UmpBuffer upper_ump;
    prepare_sidecars(lower, lower_ump);
    prepare_sidecars(upper, upper_ump);
    seed_stale_sidecars(lower, lower_ump);
    seed_stale_sidecars(upper, upper_ump);
    midi::KeyboardSplit split;
    REQUIRE(split.process(input, lower, upper).complete);
    REQUIRE(lower.sysex().size() == 1);
    REQUIRE(upper.sysex().size() == 1);
    REQUIRE(lower.ump() != nullptr);
    CHECK(lower.ump()->empty());
    REQUIRE(upper.ump() != nullptr);
    REQUIRE(upper.ump()->size() == 1);
    CHECK((*upper.ump())[0].packet.channel() == 1);
    CHECK((*upper.ump())[0].packet.note_number() == 67);
    CHECK((*upper.ump())[0].sample_offset == 11);
}

TEST_CASE("MIDI routing utilities transform native UMP channel voice events",
          "[midi][utility][routing][ump]") {
    midi::MidiBuffer input;
    midi::MidiBuffer output;
    midi::UmpBuffer input_ump;
    midi::UmpBuffer output_ump;
    prepare_sidecars(input, input_ump);
    prepare_sidecars(output, output_ump);
    REQUIRE(input_ump.add(midi::UmpPacket::note_on_2(2, 3, 59, 0xbeef), 11));

    midi::ChannelRouteSpec route;
    route.accepted_channels = std::uint16_t{1} << 3;
    route.output_channel[3] = 15;
    midi::ChannelRouter router(route);
    REQUIRE(router.process(input, output).complete);
    REQUIRE(output_ump.size() == 1);
    CHECK(output_ump[0].packet.channel() == 15);
    CHECK(output_ump[0].packet.note_number() == 59);

    midi::NoteRangeFilter range({60, 72});
    REQUIRE(range.process(input, output).complete);
    CHECK(output_ump.empty());

    midi::MidiBuffer lower;
    midi::MidiBuffer upper;
    midi::UmpBuffer lower_ump;
    midi::UmpBuffer upper_ump;
    prepare_sidecars(lower, lower_ump);
    prepare_sidecars(upper, upper_ump);
    midi::KeyboardSplit split({60, true, 2, 14});
    REQUIRE(split.process(input, lower, upper).complete);
    REQUIRE(lower_ump.size() == 1);
    CHECK(lower_ump[0].packet.channel() == 2);
    CHECK(upper_ump.empty());
}

TEST_CASE("MIDI range and split routing cover channel-wide and note-addressed expression",
          "[midi][utility][routing][ump][expression]") {
    midi::MidiBuffer input;
    midi::UmpBuffer input_ump;
    input.reserve(8, 0, 0);
    input.set_realtime_capacity_limit(true);
    input_ump.reserve(16);
    input_ump.set_realtime_capacity_limit(true);
    input.attach_ump(&input_ump);
    REQUIRE(input.add(midi::MidiEvent::cc(3, 1, 64)));
    REQUIRE(input.add(poly_pressure(3, 59, 70)));
    REQUIRE(input.add(poly_pressure(3, 60, 71)));
    REQUIRE(input_ump.add(midi1_voice(0xb0, 3, 1, 64)));
    REQUIRE(input_ump.add(midi1_voice(0xa0, 3, 59, 70)));
    REQUIRE(input_ump.add(midi::UmpPacket::cc_2(2, 3, 1, 0x12345678)));
    REQUIRE(input_ump.add(midi::UmpPacket::registered_per_note_cc(2, 3, 59, 74, 0x11111111)));
    REQUIRE(input_ump.add(midi::UmpPacket::assignable_per_note_cc(2, 3, 59, 7, 0x22222222)));
    REQUIRE(input_ump.add(midi::UmpPacket::per_note_pitch_bend(2, 3, 60, 0x87654321)));
    REQUIRE(input_ump.add(midi2_voice(0xa0, 3, 60, 0, 0x33333333)));
    REQUIRE(input_ump.add(midi::UmpPacket::per_note_management(2, 3, 60, 1)));

    midi::MidiBuffer ranged;
    midi::UmpBuffer ranged_ump;
    ranged.reserve(8, 0, 0);
    ranged.set_realtime_capacity_limit(true);
    ranged_ump.reserve(16);
    ranged_ump.set_realtime_capacity_limit(true);
    ranged.attach_ump(&ranged_ump);
    midi::NoteRangeFilter range({60, 72});
    REQUIRE(range.process(input, ranged).complete);
    REQUIRE(ranged.size() == 2);
    CHECK(ranged[0].is_cc());
    CHECK(ranged[1].note() == 60);
    REQUIRE(ranged_ump.size() == 5);
    CHECK(ranged_ump[0].packet.status() == 0xb3);
    CHECK(ranged_ump[1].packet.status() == 0xb3);
    CHECK(ranged_ump[2].packet.note_number() == 60);
    CHECK(ranged_ump[3].packet.note_number() == 60);
    CHECK(ranged_ump[4].packet.note_number() == 60);

    midi::MidiBuffer lower;
    midi::MidiBuffer upper;
    midi::UmpBuffer lower_ump;
    midi::UmpBuffer upper_ump;
    lower.reserve(8, 0, 0);
    upper.reserve(8, 0, 0);
    lower.set_realtime_capacity_limit(true);
    upper.set_realtime_capacity_limit(true);
    lower_ump.reserve(16);
    upper_ump.reserve(16);
    lower_ump.set_realtime_capacity_limit(true);
    upper_ump.set_realtime_capacity_limit(true);
    lower.attach_ump(&lower_ump);
    upper.attach_ump(&upper_ump);
    midi::KeyboardSplit split({60, true, 2, 14});
    REQUIRE(split.process(input, lower, upper).complete);
    REQUIRE(lower.size() == 2);
    REQUIRE(upper.size() == 2);
    CHECK(lower[0].channel() == 2);
    CHECK(lower[1].channel() == 2);
    CHECK(lower[1].note() == 59);
    CHECK(upper[0].channel() == 14);
    CHECK(upper[1].channel() == 14);
    CHECK(upper[1].note() == 60);
    REQUIRE(lower_ump.size() == 5);
    REQUIRE(upper_ump.size() == 5);
    for (const auto& event : lower_ump)
        CHECK(event.packet.channel() == 2);
    for (const auto& event : upper_ump)
        CHECK(event.packet.channel() == 14);
    CHECK(lower_ump[1].packet.note_number() == 59);
    CHECK(lower_ump[3].packet.note_number() == 59);
    CHECK(lower_ump[4].packet.note_number() == 59);
    CHECK(upper_ump[2].packet.note_number() == 60);
    CHECK(upper_ump[3].packet.note_number() == 60);
    CHECK(upper_ump[4].packet.note_number() == 60);
}

TEST_CASE("MIDI utilities reject aliased input and output buffers", "[midi][utility][contract]") {
    auto buffer = prepared_buffer();
    buffer.add(midi::MidiEvent::note_on(0, 60, 100));
    midi::ChannelRouter router;
    CHECK_FALSE(router.process(buffer, buffer).complete);
    CHECK(buffer.size() == 1);

    SECTION("attached UMP sidecars may not alias") {
        midi::MidiBuffer input;
        midi::MidiBuffer output;
        midi::UmpBuffer shared;
        prepare_sidecars(input, shared);
        output.reserve(4);
        output.set_realtime_capacity_limit(true);
        output.attach_ump(&shared);
        input.add(midi::MidiEvent::note_on(0, 61, 100));
        shared.add(midi::UmpPacket::note_on_2(0, 0, 61, 0xffff));
        CHECK_FALSE(router.process(input, output).complete);
        CHECK(input.size() == 1);
        CHECK(shared.size() == 1);
    }

    SECTION("split outputs may not share an attached UMP sidecar") {
        auto input = prepared_buffer();
        input.add(midi::MidiEvent::note_on(0, 61, 100));
        midi::MidiBuffer lower;
        midi::MidiBuffer upper;
        midi::UmpBuffer shared;
        prepare_sidecars(lower, shared);
        upper.reserve(4);
        upper.set_realtime_capacity_limit(true);
        upper.attach_ump(&shared);
        shared.add(midi::UmpPacket::note_on_2(0, 0, 61, 0xffff));
        midi::KeyboardSplit split;
        CHECK_FALSE(split.process(input, lower, upper).complete);
        CHECK(shared.size() == 1);
    }
}

TEST_CASE("Note length shaping orders retrigger releases before attacks",
          "[midi][utility][ordering]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    auto first = midi::MidiEvent::note_on(0, 60, 100);
    auto retrigger = midi::MidiEvent::note_on(0, 60, 110);
    first.sample_offset = 4;
    retrigger.sample_offset = 12;
    input.add(first);
    input.add(retrigger);

    midi::NoteLengthShaper<4> shaper({100});
    REQUIRE(shaper.process(input, output, {0}, 64).complete);
    REQUIRE(output.size() == 3);
    CHECK(output[0].is_note_on());
    CHECK(output[1].is_note_off());
    CHECK(output[2].is_note_on());
    CHECK(output[1].sample_offset == 12);
    CHECK(output[2].sample_offset == 12);
}

TEST_CASE("Note length shaping treats velocity-zero note-ons as releases",
          "[midi][utility][lifecycle]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    midi::NoteLengthShaper<1> shaper({4096});
    input.add(midi::MidiEvent::note_on(0, 60, 100));
    REQUIRE(shaper.process(input, output, {0}, 64).complete);
    REQUIRE(output.size() == 1);

    input.clear();
    input.add(midi::MidiEvent::note_on(0, 60, 0));
    REQUIRE(shaper.process(input, output, {64}, 64).complete);
    CHECK(output.empty());
    REQUIRE(shaper.flush(output).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].is_note_off());

    SECTION("an unprocessed velocity-zero release clears suppression debt") {
        midi::NoteLengthShaper<1> invalid({0});
        auto zero_capacity = prepared_buffer(0);
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 61, 100));
        REQUIRE_FALSE(invalid.process(input, zero_capacity, {0}, 64).complete);
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 61, 0));
        REQUIRE_FALSE(invalid.process(input, zero_capacity, {64}, 64).complete);
        REQUIRE(invalid.replace_spec({8}, output).complete);
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 61, 100));
        REQUIRE(invalid.process(input, output, {128}, 64).complete);
        REQUIRE(output.size() == 2);
    }
}

TEST_CASE("Note length saturation never schedules a release before its attack",
          "[midi][utility][ordering][boundary]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    auto attack = midi::MidiEvent::note_on(0, 60, 100);
    attack.sample_offset = 10;
    input.add(attack);
    midi::NoteLengthShaper<1> shaper({64});
    REQUIRE(
        shaper.process(input, output, {std::numeric_limits<std::int64_t>::max() - 5}, 32).complete);
    REQUIRE(output.size() == 2);
    CHECK(output[0].is_note_on());
    CHECK(output[1].is_note_off());
    CHECK(output[0].sample_offset == 10);
    CHECK(output[1].sample_offset == 10);
    NoteBalance balance;
    balance.feed(output);
    CHECK(balance.balanced());
}

TEST_CASE("Note length lifecycle operations leave no positive note balance",
          "[midi][utility][lifecycle]") {
    for (const std::string_view transition : {"stop", "seek", "loop", "reset"}) {
        CAPTURE(transition);
        auto input = prepared_buffer();
        auto output = prepared_buffer();
        midi::NoteLengthShaper<2> shaper({4096});
        NoteBalance balance;
        input.add(midi::MidiEvent::note_on(1, 64, 100));
        balance.feed(input);
        REQUIRE(shaper.process(input, output, {0}, 64).complete);
        balance = {};
        balance.feed(output);
        const auto report = transition == "reset" ? shaper.reset(output) : shaper.flush(output);
        REQUIRE(report.complete);
        balance.feed(output);
        CHECK(balance.balanced());
    }

    SECTION("spec swap flushes the old note before adopting the new length") {
        auto input = prepared_buffer();
        auto output = prepared_buffer();
        midi::NoteLengthShaper<2> shaper({4096});
        NoteBalance balance;
        input.add(midi::MidiEvent::note_on(0, 60, 100));
        shaper.process(input, output, {0}, 64);
        balance.feed(output);
        REQUIRE(shaper.replace_spec({8}, output).complete);
        balance.feed(output);
        CHECK(balance.balanced());
    }

    SECTION("a valid spec repairs an invalid construction") {
        auto input = prepared_buffer();
        auto output = prepared_buffer();
        midi::NoteLengthShaper<2> shaper({0});
        REQUIRE_FALSE(shaper.valid());
        REQUIRE(shaper.replace_spec({8}, output).complete);
        REQUIRE(shaper.valid());

        input.add(midi::MidiEvent::note_on(0, 60, 100));
        REQUIRE(shaper.process(input, output, {0}, 64).complete);
        REQUIRE(output.size() == 2);
        CHECK(output[0].is_note_on());
        CHECK(output[1].is_note_off());
        CHECK(output[1].sample_offset == 8);
    }
}

TEST_CASE("Note length overflow remains balanced and processing remains allocation free",
          "[midi][utility][overflow][rt-safety]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    input.add(midi::MidiEvent::note_on(0, 60, 100));
    input.add(midi::MidiEvent::note_on(0, 61, 100));
    midi::NoteLengthShaper<1> shaper({4096});
    NoteBalance balance;

    pulp::test::RtAllocationProbe allocations;
    const auto report = shaper.process(input, output, {0}, 64);
    CHECK_FALSE(allocations.saw_allocation());
    REQUIRE(report.complete);
    balance.feed(output);
    REQUIRE(shaper.flush(output).complete);
    balance.feed(output);
    CHECK(balance.balanced());

    SECTION("a failed retrigger release suppresses the new attack") {
        auto retriggers = prepared_buffer();
        auto one_event_output = prepared_buffer(1);
        retriggers.add(midi::MidiEvent::note_on(0, 70, 100));
        retriggers.add(midi::MidiEvent::note_on(0, 70, 110));
        midi::NoteLengthShaper<2> retrigger_shaper({4096});
        NoteBalance retrigger_balance;
        CHECK_FALSE(retrigger_shaper.process(retriggers, one_event_output, {0}, 64).complete);
        REQUIRE(one_event_output.size() == 1);
        retrigger_balance.feed(one_event_output);
        REQUIRE(retrigger_shaper.flush(output).complete);
        retrigger_balance.feed(output);
        CHECK(retrigger_balance.balanced());
    }

    SECTION("release debt blocks later flush work until it is emitted") {
        auto attacks = prepared_buffer();
        attacks.add(midi::MidiEvent::note_on(0, 60, 100));
        attacks.add(midi::MidiEvent::note_on(0, 61, 100));
        midi::NoteLengthShaper<1> debt_shaper({4096});
        NoteBalance debt_balance;
        REQUIRE(debt_shaper.process(attacks, output, {0}, 64).complete);
        debt_balance.feed(output);

        auto release = prepared_buffer();
        release.add(midi::MidiEvent::note_off(0, 61));
        auto zero_capacity = prepared_buffer(0);
        CHECK_FALSE(debt_shaper.process(release, zero_capacity, {64}, 64).complete);
        CHECK_FALSE(debt_shaper.flush(zero_capacity).complete);
        REQUIRE(debt_shaper.flush(output).complete);
        debt_balance.feed(output);
        CHECK(debt_balance.balanced());
    }
}

TEST_CASE("Note length fail-open ownership exceeds uint16 depth without losing balance",
          "[midi][utility][overflow][amplification]") {
    constexpr std::size_t attacks = 65536;
    auto input = prepared_buffer(attacks);
    auto output = prepared_buffer(attacks);
    for (std::size_t i = 0; i < attacks; ++i)
        REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    midi::NoteLengthShaper<0> shaper({4096});
    const auto process_report = shaper.process(input, output, {0}, 64);
    REQUIRE(process_report.complete);
    CHECK(process_report.emitted == attacks);
    CHECK(process_report.emitted <= input.size() * shaper.contract().maximum_event_amplification);
    NoteBalance balance;
    balance.feed(output);
    const auto flush_report = shaper.flush(output);
    REQUIRE(flush_report.complete);
    CHECK(flush_report.emitted == attacks);
    balance.feed(output);
    CHECK(balance.balanced());
}

TEST_CASE("Monophonic priorities maintain balanced release-before-attack transitions",
          "[midi][utility][mono]") {
    for (const auto priority : {midi::MonophonicPriority::Low, midi::MonophonicPriority::High,
                                midi::MonophonicPriority::Last}) {
        auto input = prepared_buffer();
        auto output = prepared_buffer();
        midi::MonophonicNoteSelector selector({priority, true, 0.01});
        auto high = midi::MidiEvent::note_on(0, 72, 100);
        auto low = midi::MidiEvent::note_on(0, 48, 100);
        low.sample_offset = 7;
        input.add(high);
        input.add(low);
        REQUIRE(selector.process(input, output).complete);
        REQUIRE(output.size() >= 1);
        const auto expected = priority == midi::MonophonicPriority::High ? 72 : 48;
        CHECK(selector.pitch_state().note == expected);
        if (output.size() == 3) {
            CHECK(output[1].is_note_off());
            CHECK(output[2].is_note_on());
            CHECK(output[1].sample_offset == output[2].sample_offset);
        }
        NoteBalance balance;
        balance.feed(output);
        REQUIRE(selector.flush(output).complete);
        balance.feed(output);
        CHECK(balance.balanced());
    }
}

TEST_CASE("Monophonic selection treats velocity-zero note-ons as releases",
          "[midi][utility][mono][lifecycle]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    midi::MonophonicNoteSelector selector;
    input.add(midi::MidiEvent::note_on(0, 60, 100));
    REQUIRE(selector.process(input, output).complete);
    REQUIRE(selector.pitch_state().active);

    input.clear();
    input.add(midi::MidiEvent::note_on(0, 60, 0));
    REQUIRE(selector.process(input, output).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].is_note_off());
    CHECK_FALSE(selector.pitch_state().active);
}

TEST_CASE("Last-note priority rebases deterministically across serial rollover",
          "[midi][utility][mono][ordering]") {
    STATIC_CHECK_FALSE(midi::monophonic_serial_capacity_supported<std::uint8_t>);
    STATIC_CHECK(midi::monophonic_serial_capacity_supported<std::uint16_t>);
    midi::BasicMonophonicNoteSelector<std::uint16_t> selector(
        {midi::MonophonicPriority::Last, true, 0.0});
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    input.add(midi::MidiEvent::note_on(0, 40, 100));
    REQUIRE(selector.process(input, output).complete);
    for (std::uint32_t i = 0; i < std::numeric_limits<std::uint16_t>::max() - 1; ++i) {
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 60, 100));
        REQUIRE(selector.process(input, output).complete);
        input.clear();
        input.add(midi::MidiEvent::note_off(0, 60));
        REQUIRE(selector.process(input, output).complete);
    }
    input.clear();
    input.add(midi::MidiEvent::note_on(0, 80, 100));
    REQUIRE(selector.process(input, output).complete);
    CHECK(selector.pitch_state().note == 80);
}

TEST_CASE("Monophonic held ownership remains exact beyond uint16 depth",
          "[midi][utility][mono][overflow]") {
    constexpr std::size_t attacks = 65'536;
    auto input = prepared_buffer(attacks);
    auto output = prepared_buffer(2);
    for (std::size_t i = 0; i < attacks; ++i)
        REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));

    midi::MonophonicNoteSelector selector;
    NoteBalance balance;
    const auto attack_report = selector.process(input, output);
    REQUIRE(attack_report.complete);
    REQUIRE(attack_report.emitted == 1);
    balance.feed(output);

    input.clear();
    for (std::size_t i = 0; i < attacks - 1; ++i)
        REQUIRE(input.add(midi::MidiEvent::note_off(0, 60)));
    REQUIRE(selector.process(input, output).complete);
    CHECK(output.empty());
    CHECK(selector.pitch_state().active);

    input.clear();
    REQUIRE(input.add(midi::MidiEvent::note_off(0, 60)));
    REQUIRE(selector.process(input, output).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].is_note_off());
    balance.feed(output);
    CHECK(balance.balanced());
}

TEST_CASE("Controller mapping applies caller-owned curves, smoothing, and legal CC domains",
          "[midi][utility][controller]") {
    midi::ControllerMapper<2> mapper;
    CHECK_FALSE(mapper.set_rule(0, {.input_channel = 16, .enabled = true}));
    CHECK_FALSE(mapper.set_rule(0, {.output_cc = 128, .enabled = true}));
    REQUIRE(mapper.set_rule(0, {.input_channel = 2,
                                .input_cc = 1,
                                .output_channel = 15,
                                .output_cc = 74,
                                .minimum = 0.0f,
                                .maximum = 1.0f,
                                .smoothing_seconds = 0.01f,
                                .curve = square_curve,
                                .enabled = true}));
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    input.add(midi::MidiEvent::cc(2, 1, 64));
    REQUIRE(mapper.process(input, output, 48000.0).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].channel() == 15);
    CHECK(output[0].cc_number() == 74);
    CHECK(output[0].cc_value() <= 127);
    CHECK(mapper.value(0) == Catch::Approx((64.0f / 127.0f) * (64.0f / 127.0f)));

    SECTION("non-default ranges scale the emitted CC value") {
        midi::ControllerMapper<1> ranged;
        REQUIRE(ranged.set_rule(0, {.input_channel = 2,
                                    .input_cc = 1,
                                    .output_channel = 15,
                                    .output_cc = 74,
                                    .minimum = 0.5f,
                                    .maximum = 1.0f,
                                    .enabled = true}));
        auto low = prepared_buffer();
        low.add(midi::MidiEvent::cc(2, 1, 0));
        REQUIRE(ranged.process(low, output, 48000.0).complete);
        REQUIRE(output.size() == 1);
        CHECK(output[0].cc_value() == 64);
    }

    SECTION("non-finite clock and curve values fail closed") {
        CHECK_FALSE(
            mapper.process(input, output, std::numeric_limits<double>::quiet_NaN()).complete);
        REQUIRE(mapper.set_rule(0, {.input_channel = 2,
                                    .input_cc = 1,
                                    .output_channel = 15,
                                    .output_cc = 74,
                                    .minimum = 0.0f,
                                    .maximum = 1.0f,
                                    .curve = nan_curve,
                                    .enabled = true}));
        CHECK_FALSE(
            mapper.process(input, output, 48000.0, {std::numeric_limits<std::int64_t>::max()})
                .complete);
        CHECK(output.empty());
    }

    SECTION("extreme finite domains and sample positions remain defined") {
        midi::ControllerMapper<1> extreme;
        REQUIRE(extreme.set_rule(0, {.input_channel = 2,
                                     .input_cc = 1,
                                     .output_channel = 3,
                                     .output_cc = 7,
                                     .minimum = -std::numeric_limits<float>::max(),
                                     .maximum = std::numeric_limits<float>::max(),
                                     .smoothing_seconds = 0.01f,
                                     .enabled = true}));
        auto low = prepared_buffer();
        low.add(midi::MidiEvent::cc(2, 1, 0));
        REQUIRE(extreme.process(low, output, 48000.0, {std::numeric_limits<std::int64_t>::min()})
                    .complete);
        auto high = prepared_buffer();
        high.add(midi::MidiEvent::cc(2, 1, 127));
        REQUIRE(extreme.process(high, output, 48000.0, {std::numeric_limits<std::int64_t>::max()})
                    .complete);
        REQUIRE(output.size() == 1);
        CHECK(output[0].cc_value() == 127);
        CHECK(std::isfinite(extreme.value(0)));
    }
}

TEST_CASE("Scale-aware MPE bend uses Pulp scale arithmetic with Forge tie parity",
          "[midi][utility][mpe][parity]") {
    const auto scale = music::Scale::named(music::PitchClass::c, music::NamedScale::major);
    REQUIRE(scale.has_value());
    midi::MpeNoteState note;
    note.active = true;
    note.note = 61;
    note.pitch_bend_semitones = 0.0f;
    midi::ScaleAwareMpePitch pitch;
    CHECK(pitch.target_cents(note, *scale) == Catch::Approx(-100.0f));

    note.note = 60;
    note.pitch_bend_semitones = 24.0f;
    CHECK(pitch.target_cents(note, *scale) == Catch::Approx(200.0f));

    note.pitch_bend_semitones = std::numeric_limits<float>::quiet_NaN();
    CHECK(std::isfinite(pitch.target_cents(note, *scale)));
    pitch.replace_spec({.input_bend_range_semitones = 48.0f,
                        .bend_range_degrees = std::numeric_limits<float>::max(),
                        .glide_seconds = 0.0f});
    note.pitch_bend_semitones = std::numeric_limits<float>::max();
    CHECK(std::isfinite(pitch.target_cents(note, *scale)));
}
