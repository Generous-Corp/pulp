#pragma once

#include <pulp/midi/block_ops.hpp>
#include <pulp/midi/mpe_voice_tracker.hpp>
#include <pulp/music/pitch.hpp>
#include <pulp/timebase/rational_time.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace pulp::midi {

enum class MidiUtilityOverflowPolicy : std::uint8_t {
    DropUnstarted,
    FailOpenBalanced,
    RetainReleaseDebt,
};

enum class MidiUtilitySameSampleOrder : std::uint8_t {
    InputStable,
    ReleaseBeforeAttack,
};

enum class MidiUtilityTransportRequirement : std::uint8_t {
    None,
    MonotonicSamples,
    FlushOnDiscontinuity,
};

struct MidiUtilityContract {
    std::size_t maximum_event_amplification = 1;
    std::size_t state_capacity = 0;
    MidiUtilityOverflowPolicy overflow = MidiUtilityOverflowPolicy::DropUnstarted;
    MidiUtilitySameSampleOrder same_sample_order = MidiUtilitySameSampleOrder::InputStable;
    MidiUtilityTransportRequirement transport = MidiUtilityTransportRequirement::None;
    bool requires_reserved_capacity_limited_output = true;
    bool requires_distinct_input_output = true;
};

struct MidiUtilityProcessReport {
    std::size_t emitted = 0;
    std::size_t dropped = 0;
    std::size_t deferred = 0;
    bool complete = true;
};

namespace utility_detail {

inline int key_index(std::uint8_t channel, std::uint8_t note) noexcept {
    return static_cast<int>(channel & 0x0f) * 128 + static_cast<int>(note & 0x7f);
}

constexpr std::int64_t saturating_sample_add(std::int64_t position, std::int64_t offset) noexcept {
    if (offset > 0 && position > std::numeric_limits<std::int64_t>::max() - offset)
        return std::numeric_limits<std::int64_t>::max();
    if (offset < 0 && position < std::numeric_limits<std::int64_t>::min() - offset)
        return std::numeric_limits<std::int64_t>::min();
    return position + offset;
}

inline bool emit(MidiBuffer& output, const MidiEvent& event, MidiUtilityProcessReport& report) {
    if (!output.realtime_capacity_limited()) {
        ++report.dropped;
        report.complete = false;
        return false;
    }
    if (output.add(event)) {
        ++report.emitted;
        return true;
    }
    ++report.dropped;
    report.complete = false;
    return false;
}

inline void clear_output(MidiBuffer& output) noexcept {
    clear_midi_block(output);
}

inline bool ready(const MidiBuffer& output) noexcept {
    const auto* ump = output.ump();
    return output.realtime_capacity_limited() &&
           (ump == nullptr || ump->realtime_capacity_limited());
}

inline bool blocks_alias(const MidiBuffer& first, const MidiBuffer& second) noexcept {
    return &first == &second || (first.ump() != nullptr && first.ump() == second.ump());
}

inline void copy_sidecars(const MidiBuffer& input, MidiBuffer& output,
                          MidiUtilityProcessReport& report) noexcept {
    if (!copy_midi_sidecars(input, output)) {
        ++report.dropped;
        report.complete = false;
    }
}

inline bool copy_sysex_sidecar(const MidiBuffer& input, MidiBuffer& output) noexcept {
    bool copied_all = !midi_block_has_drops(input);
    for (const auto& sx : input.sysex()) {
        const bool copied = sx.data.empty() ? output.add_sysex({}, sx.sample_offset, sx.timestamp)
                                            : output.add_sysex_copy(sx.data.data(), sx.data.size(),
                                                                    sx.sample_offset, sx.timestamp);
        copied_all = copied && copied_all;
    }
    return copied_all;
}

inline bool emit_ump(UmpBuffer* output, const UmpEvent& event) noexcept {
    return output != nullptr && output->add(event);
}

inline bool is_channel_voice(const UmpPacket& packet) noexcept {
    return packet.message_type() == UmpMessageType::Midi1ChannelVoice ||
           packet.message_type() == UmpMessageType::Midi2ChannelVoice;
}

inline bool is_note_addressed(const UmpPacket& packet) noexcept {
    if (!is_channel_voice(packet))
        return false;
    const auto status = static_cast<std::uint8_t>(packet.status() & 0xf0);
    if (packet.message_type() == UmpMessageType::Midi1ChannelVoice)
        return status == 0x80 || status == 0x90 || status == 0xa0;
    return status == 0x00 || status == 0x10 || status == 0x60 || status == 0x80 || status == 0x90 ||
           status == 0xa0 || status == 0xf0;
}

inline UmpPacket with_channel(UmpPacket packet, std::uint8_t channel) noexcept {
    packet.words[0] = (packet.words[0] & ~(std::uint32_t{0x0f} << 16)) |
                      (static_cast<std::uint32_t>(channel & 0x0f) << 16);
    return packet;
}

inline MidiEvent at(MidiEvent event, std::int32_t sample_offset) noexcept {
    event.sample_offset = sample_offset;
    return event;
}

inline MidiEvent with_channel(const MidiEvent& event, std::uint8_t channel) noexcept {
    const auto* bytes = event.data();
    const auto status = static_cast<std::uint8_t>((bytes[0] & 0xf0) | (channel & 0x0f));
    MidiEvent result{choc::midi::ShortMessage(status, event.size() > 1 ? bytes[1] : 0,
                                              event.size() > 2 ? bytes[2] : 0),
                     event.sample_offset, event.timestamp};
    return result;
}

inline bool is_channel_voice(const MidiEvent& event) noexcept {
    return event.size() != 0 && event.data()[0] >= 0x80 && event.data()[0] < 0xf0;
}

inline bool is_note_addressed(const MidiEvent& event) noexcept {
    if (!is_channel_voice(event))
        return false;
    const auto status = static_cast<std::uint8_t>(event.data()[0] & 0xf0);
    return status == 0x80 || status == 0x90 || status == 0xa0;
}

inline int nearest_scale_note(int note, const music::Scale& scale) noexcept {
    int best = std::clamp(note, 0, 127);
    int best_distance = std::numeric_limits<int>::max();
    for (int candidate = 0; candidate <= 127; ++candidate) {
        if (!scale.contains(static_cast<music::PitchClass>(candidate % 12)))
            continue;
        const int distance = std::abs(candidate - note);
        if (distance < best_distance || (distance == best_distance && candidate < best)) {
            best = candidate;
            best_distance = distance;
        }
    }
    return best;
}

} // namespace utility_detail

} // namespace pulp::midi
