#include "midi_utility_test_support.hpp"

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
        REQUIRE(router.reset(output).complete);
        REQUIRE(output.size() == 1);
        CHECK(output[0].is_note_off());
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
        auto no_capacity = prepared_buffer(0);
        CHECK_FALSE(router.reset(no_capacity).complete);
        CHECK_FALSE(router.replace_spec(replacement));
        REQUIRE(router.reset(constrained).complete);
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

    SECTION("a later suppressed attack cannot hide an older routed release") {
        midi::ChannelRouteSpec initial;
        initial.accepted_channels = std::uint16_t{1} << 3;
        initial.output_channel[3] = 15;
        midi::ChannelRouter router(initial);
        auto constrained = prepared_buffer(1);

        input.add(midi::MidiEvent::note_on(3, 64, 100));
        REQUIRE(router.process(input, constrained).complete);
        input.clear();
        input.add(midi::MidiEvent::cc(3, 7, 100));
        input.add(midi::MidiEvent::note_on(3, 64, 110));
        CHECK_FALSE(router.process(input, constrained).complete);

        input.clear();
        input.add(midi::MidiEvent::note_off(3, 64));
        REQUIRE(router.process(input, constrained).complete);
        REQUIRE(constrained.size() == 1);
        CHECK(constrained[0].is_note_off());
        CHECK(constrained[0].channel() == 15);

        REQUIRE(router.process(input, constrained).complete);
        CHECK(constrained.empty());
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

TEST_CASE("routing lifecycle flushes owned notes and suppresses stale releases",
          "[midi][utility][routing][lifecycle][ownership]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();

    midi::ChannelRouteSpec initial;
    initial.accepted_channels = std::uint16_t{1} << 3;
    initial.output_channel[3] = 15;
    auto replacement = initial;
    replacement.output_channel[3] = 4;
    midi::ChannelRouter router(initial);

    input.add(midi::MidiEvent::note_on(3, 64, 100));
    REQUIRE(router.process(input, output).complete);
    REQUIRE(router.replace_spec(replacement, output).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].is_note_off());
    CHECK(output[0].channel() == 15);

    input.clear();
    input.add(midi::MidiEvent::note_off(3, 64));
    REQUIRE(router.process(input, output).complete);
    CHECK(output.empty());

    input.clear();
    input.add(midi::MidiEvent::note_on(3, 65, 100));
    input.add(midi::MidiEvent::note_off(3, 65));
    REQUIRE(router.process(input, output).complete);
    REQUIRE(output.size() == 2);
    CHECK(output[0].channel() == 4);
    CHECK(output[1].channel() == 4);

    SECTION("range reset emits the owned release") {
        midi::NoteRangeFilter range({60, 72});
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 64, 100));
        REQUIRE(range.process(input, output).complete);
        REQUIRE(range.reset(output).complete);
        REQUIRE(output.size() == 1);
        CHECK(output[0].is_note_off());
        CHECK(output[0].note() == 64);
    }

    SECTION("split reset emits each release through its accepting side") {
        midi::KeyboardSplit split({60, true, 2, 14});
        auto lower = prepared_buffer();
        auto upper = prepared_buffer();
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 59, 100));
        input.add(midi::MidiEvent::note_on(0, 60, 100));
        REQUIRE(split.process(input, lower, upper).complete);
        REQUIRE(split.reset(lower, upper).complete);
        REQUIRE(lower.size() == 1);
        REQUIRE(upper.size() == 1);
        CHECK(lower[0].is_note_off());
        CHECK(lower[0].channel() == 2);
        CHECK(upper[0].is_note_off());
        CHECK(upper[0].channel() == 14);
    }

    SECTION("UMP hot swap keeps protocol, group, route, and stale-release ownership") {
        midi::ChannelRouter ump_router(initial);
        midi::UmpBuffer input_ump;
        midi::UmpBuffer output_ump;
        prepare_sidecars(input, input_ump);
        prepare_sidecars(output, output_ump);
        input.clear();
        REQUIRE(input_ump.add(midi::UmpPacket::note_on_2(5, 3, 66, 0x8000), 7));
        REQUIRE(ump_router.process(input, output).complete);
        REQUIRE(output_ump.size() == 1);
        REQUIRE(ump_router.replace_spec(replacement, output).complete);
        REQUIRE(output_ump.size() == 1);
        CHECK(output_ump[0].packet.group() == 5);
        CHECK(output_ump[0].packet.channel() == 15);
        CHECK((output_ump[0].packet.status() & 0xf0) == 0x80);

        input_ump.clear();
        REQUIRE(input_ump.add(midi::UmpPacket::note_off_2(5, 3, 66), 0));
        REQUIRE(ump_router.process(input, output).complete);
        CHECK(output_ump.empty());
    }

    SECTION("bounded flush retries preserve every release") {
        constexpr std::size_t note_count = 40;
        auto attacks = prepared_buffer(note_count);
        auto routed = prepared_buffer(note_count);
        midi::ChannelRouter many;
        for (std::size_t i = 0; i < note_count; ++i)
            REQUIRE(attacks.add(midi::MidiEvent::note_on(
                0, static_cast<std::uint8_t>(i), 100)));
        REQUIRE(many.process(attacks, routed).complete);
        NoteBalance balance;
        balance.feed(routed);

        auto bounded = prepared_buffer(16);
        std::size_t flush_calls = 0;
        for (;;) {
            const auto report = many.flush(bounded);
            ++flush_calls;
            balance.feed(bounded);
            if (report.complete)
                break;
            REQUIRE(report.deferred > 0);
            REQUIRE(flush_calls < 8);
        }
        CHECK(flush_calls == 3);
        CHECK(balance.balanced());

        attacks.clear();
        for (std::size_t i = 0; i < note_count; ++i)
            REQUIRE(attacks.add(midi::MidiEvent::note_off(
                0, static_cast<std::uint8_t>(i))));
        REQUIRE(many.process(attacks, routed).complete);
        CHECK(routed.empty());
    }

    SECTION("a rejecting route still consumes the old release before reuse") {
        midi::ChannelRouter changing(initial);
        input.clear();
        input.add(midi::MidiEvent::note_on(3, 67, 100));
        REQUIRE(changing.process(input, output).complete);

        auto rejecting = initial;
        rejecting.accepted_channels &= ~(std::uint16_t{1} << 3);
        REQUIRE(changing.replace_spec(rejecting, output).complete);
        REQUIRE(output.size() == 1);
        CHECK(output[0].is_note_off());
        CHECK(output[0].channel() == 15);

        input.clear();
        input.add(midi::MidiEvent::note_off(3, 67));
        REQUIRE(changing.process(input, output).complete);
        CHECK(output.empty());
        REQUIRE(changing.replace_spec(replacement));

        input.clear();
        input.add(midi::MidiEvent::note_on(3, 67, 100));
        input.add(midi::MidiEvent::note_off(3, 67));
        REQUIRE(changing.process(input, output).complete);
        REQUIRE(output.size() == 2);
        CHECK(output[0].is_note_on());
        CHECK(output[1].is_note_off());
        CHECK(output[0].channel() == 4);
        CHECK(output[1].channel() == 4);
    }

    SECTION("a rejecting range still consumes the old release before reuse") {
        midi::NoteRangeFilter changing({60, 69});
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 64, 100));
        REQUIRE(changing.process(input, output).complete);
        REQUIRE(changing.replace_spec({70, 80}, output).complete);
        REQUIRE(output.size() == 1);
        CHECK(output[0].is_note_off());

        input.clear();
        input.add(midi::MidiEvent::note_off(0, 64));
        REQUIRE(changing.process(input, output).complete);
        CHECK(output.empty());
        REQUIRE(changing.replace_spec({60, 69}));

        input.clear();
        input.add(midi::MidiEvent::note_on(0, 64, 100));
        input.add(midi::MidiEvent::note_off(0, 64));
        REQUIRE(changing.process(input, output).complete);
        REQUIRE(output.size() == 2);
        CHECK(output[0].is_note_on());
        CHECK(output[1].is_note_off());
    }

    SECTION("a rejecting UMP route consumes the old release before reuse") {
        midi::ChannelRouter changing(initial);
        midi::UmpBuffer input_ump;
        midi::UmpBuffer output_ump;
        prepare_sidecars(input, input_ump);
        prepare_sidecars(output, output_ump);
        input.clear();
        REQUIRE(input_ump.add(midi::UmpPacket::note_on_2(5, 3, 68, 0x8000), 4));
        REQUIRE(changing.process(input, output).complete);

        auto rejecting = initial;
        rejecting.accepted_channels &= ~(std::uint16_t{1} << 3);
        REQUIRE(changing.replace_spec(rejecting, output).complete);
        REQUIRE(output_ump.size() == 1);

        input_ump.clear();
        REQUIRE(input_ump.add(midi::UmpPacket::note_off_2(5, 3, 68), 5));
        REQUIRE(changing.process(input, output).complete);
        CHECK(output_ump.empty());
        REQUIRE(changing.replace_spec(replacement));

        input_ump.clear();
        REQUIRE(input_ump.add(midi::UmpPacket::note_on_2(5, 3, 68, 0x8000), 6));
        REQUIRE(input_ump.add(midi::UmpPacket::note_off_2(5, 3, 68), 7));
        REQUIRE(changing.process(input, output).complete);
        REQUIRE(output_ump.size() == 2);
        CHECK((output_ump[0].packet.status() & 0xf0) == 0x90);
        CHECK((output_ump[1].packet.status() & 0xf0) == 0x80);
        CHECK(output_ump[0].packet.channel() == 4);
        CHECK(output_ump[1].packet.channel() == 4);
    }

    SECTION("a newly accepted route does not forward a previously rejected release") {
        auto rejecting = initial;
        rejecting.accepted_channels &= ~(std::uint16_t{1} << 3);
        midi::ChannelRouter changing(rejecting);
        input.clear();
        input.add(midi::MidiEvent::note_on(3, 69, 100));
        REQUIRE(changing.process(input, output).complete);
        CHECK(output.empty());
        REQUIRE(changing.replace_spec(replacement, output).complete);

        input.clear();
        input.add(midi::MidiEvent::note_off(3, 69));
        REQUIRE(changing.process(input, output).complete);
        CHECK(output.empty());
    }

    SECTION("a newly accepted range does not forward a previously rejected release") {
        midi::NoteRangeFilter changing({70, 80});
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 64, 100));
        REQUIRE(changing.process(input, output).complete);
        CHECK(output.empty());
        REQUIRE(changing.replace_spec({60, 69}, output).complete);

        input.clear();
        input.add(midi::MidiEvent::note_off(0, 64));
        REQUIRE(changing.process(input, output).complete);
        CHECK(output.empty());
    }

    SECTION("a newly accepted UMP route consumes a previously rejected release") {
        auto rejecting = initial;
        rejecting.accepted_channels &= ~(std::uint16_t{1} << 3);
        midi::ChannelRouter changing(rejecting);
        midi::UmpBuffer input_ump;
        midi::UmpBuffer output_ump;
        prepare_sidecars(input, input_ump);
        prepare_sidecars(output, output_ump);
        input.clear();
        REQUIRE(input_ump.add(midi::UmpPacket::note_on_2(5, 3, 69, 0x8000), 4));
        REQUIRE(changing.process(input, output).complete);
        CHECK(output_ump.empty());
        REQUIRE(changing.replace_spec(replacement, output).complete);

        input_ump.clear();
        REQUIRE(input_ump.add(midi::UmpPacket::note_off_2(5, 3, 69), 5));
        REQUIRE(changing.process(input, output).complete);
        CHECK(output_ump.empty());
    }
}

TEST_CASE("routing kernels retain ownership when output preparation rejects a block",
          "[midi][utility][routing][overflow][ownership]") {
    auto input = prepared_buffer();
    midi::MidiBuffer unprepared;
    auto output = prepared_buffer();

    SECTION("channel router") {
        midi::ChannelRouter router;
        input.add(midi::MidiEvent::note_on(0, 60, 100));
        CHECK_FALSE(router.process(input, unprepared).complete);
        input.clear();
        input.add(midi::MidiEvent::note_off(0, 60));
        REQUIRE(router.process(input, output).complete);
        CHECK(output.empty());
    }

    SECTION("note range") {
        midi::NoteRangeFilter range;
        input.add(midi::MidiEvent::note_on(0, 60, 100));
        CHECK_FALSE(range.process(input, unprepared).complete);
        input.clear();
        input.add(midi::MidiEvent::note_off(0, 60));
        REQUIRE(range.process(input, output).complete);
        CHECK(output.empty());
    }

    SECTION("keyboard split") {
        midi::KeyboardSplit split;
        auto upper = prepared_buffer();
        input.add(midi::MidiEvent::note_on(0, 60, 100));
        CHECK_FALSE(split.process(input, unprepared, upper).complete);
        auto lower = prepared_buffer();
        input.clear();
        input.add(midi::MidiEvent::note_off(0, 60));
        REQUIRE(split.process(input, lower, upper).complete);
        CHECK(lower.empty());
        CHECK(upper.empty());
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
