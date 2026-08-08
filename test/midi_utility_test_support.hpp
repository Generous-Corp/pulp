#pragma once

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

[[maybe_unused]] midi::MidiBuffer prepared_buffer(std::size_t capacity = 64) {
    midi::MidiBuffer buffer;
    buffer.reserve(capacity);
    buffer.set_realtime_capacity_limit(true);
    return buffer;
}

[[maybe_unused]] float square_curve(float value, float) noexcept {
    return value * value;
}

[[maybe_unused]] float nan_curve(float, float) noexcept {
    return std::numeric_limits<float>::quiet_NaN();
}

[[maybe_unused]] void prepare_sidecars(midi::MidiBuffer& buffer, midi::UmpBuffer& ump) {
    buffer.reserve(64, 2, 8);
    buffer.set_realtime_capacity_limit(true);
    ump.reserve(2);
    ump.set_realtime_capacity_limit(true);
    buffer.attach_ump(&ump);
}

[[maybe_unused]] void seed_input_sidecars(midi::MidiBuffer& buffer, midi::UmpBuffer& ump) {
    const std::array<std::uint8_t, 4> payload{0xf0, 0x7d, 0x01, 0xf7};
    REQUIRE(buffer.add_sysex_copy(payload.data(), payload.size(), 9, 0.25));
    REQUIRE(ump.add(midi::UmpPacket::note_on_2(2, 3, 67, 0xbeef), 11));
}

[[maybe_unused]] void seed_stale_sidecars(midi::MidiBuffer& buffer, midi::UmpBuffer& ump) {
    const std::array<std::uint8_t, 2> stale{0xf0, 0xf7};
    REQUIRE(buffer.add_sysex_copy(stale.data(), stale.size(), 1));
    REQUIRE(ump.add(midi::UmpPacket::note_off_2(0, 0, 1), 1));
}

[[maybe_unused]] midi::MidiEvent poly_pressure(std::uint8_t channel, std::uint8_t note,
                                               std::uint8_t value) {
    return {
        choc::midi::ShortMessage(static_cast<std::uint8_t>(0xa0 | (channel & 0x0f)), note, value),
        0, 0.0};
}

[[maybe_unused]] midi::UmpPacket midi1_voice(std::uint8_t status, std::uint8_t channel,
                                             std::uint8_t data1, std::uint8_t data2,
                                             std::uint8_t group = 0) {
    midi::UmpPacket packet;
    packet.word_count = 1;
    packet.words[0] = (0x2u << 28) | (static_cast<std::uint32_t>(group & 0x0f) << 24) |
                      (static_cast<std::uint32_t>(status | (channel & 0x0f)) << 16) |
                      (static_cast<std::uint32_t>(data1) << 8) | data2;
    return packet;
}

[[maybe_unused]] midi::UmpPacket midi2_voice(std::uint8_t status, std::uint8_t channel,
                                             std::uint8_t data1, std::uint8_t data2,
                                             std::uint32_t value) {
    midi::UmpPacket packet;
    packet.word_count = 2;
    packet.words[0] = (0x4u << 28) | (static_cast<std::uint32_t>(status | (channel & 0x0f)) << 16) |
                      (static_cast<std::uint32_t>(data1) << 8) | data2;
    packet.words[1] = value;
    return packet;
}

[[maybe_unused]] void check_exact_sidecars(const midi::MidiBuffer& buffer) {
    REQUIRE(buffer.sysex().size() == 1);
    CHECK(buffer.sysex()[0].data == std::vector<std::uint8_t>{0xf0, 0x7d, 0x01, 0xf7});
    CHECK(buffer.sysex()[0].sample_offset == 9);
    REQUIRE(buffer.ump() != nullptr);
    REQUIRE(buffer.ump()->size() == 1);
    CHECK((*buffer.ump())[0].packet.words == midi::UmpPacket::note_on_2(2, 3, 67, 0xbeef).words);
    CHECK((*buffer.ump())[0].sample_offset == 11);
}

} // namespace
