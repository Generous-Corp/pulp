#pragma once

/// @file mpe_config_messages.hpp
/// Builders for the MIDI messages that configure an MPE device: the MPE
/// Configuration Message (zone member counts) and the pitch-bend-range
/// Registered Parameter. `MpeVoiceTracker` understands the same messages on
/// input, so a sender built with these helpers and a tracker round-trip.

#include <pulp/midi/device.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/ump.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace pulp::midi {

/// Fixed-capacity, allocation-free list of events returned by the builders.
template <std::size_t Capacity>
struct MpeMessageList {
    std::array<MidiEvent, Capacity> events{};
    std::size_t count = 0;

    const MidiEvent* begin() const { return events.data(); }
    const MidiEvent* end() const { return events.data() + count; }
    std::size_t size() const { return count; }

    void push(const MidiEvent& e) {
        if (count < Capacity) events[count++] = e;
    }
};

/// MPE Configuration Message: RPN 6 on a manager channel. `manager_channel` is
/// 0 for the lower zone or 15 for the upper zone; `member_count` is 0-15 and 0
/// disables the zone.
inline MpeMessageList<3> mpe_configuration_message(uint8_t manager_channel, uint8_t member_count) {
    MpeMessageList<3> out;
    const uint8_t ch = manager_channel == 15 ? uint8_t{15} : uint8_t{0};
    out.push(MidiEvent::cc(ch, 101, 0));
    out.push(MidiEvent::cc(ch, 100, 6));
    out.push(MidiEvent::cc(ch, 6, member_count > 15 ? uint8_t{15} : member_count));
    return out;
}

/// Pitch-bend range (RPN 0) for one channel: whole semitones in Data Entry MSB
/// and the remaining hundredths in Data Entry LSB. Ranges above 127 semitones
/// are clamped and negative ranges become zero.
inline MpeMessageList<4> mpe_pitch_bend_range_message(uint8_t channel, float semitones) {
    MpeMessageList<4> out;
    if (!(semitones > 0.0f)) semitones = 0.0f;
    if (semitones > 127.0f) semitones = 127.0f;
    const auto whole = static_cast<uint8_t>(semitones);
    auto cents = static_cast<int>((semitones - static_cast<float>(whole)) * 100.0f + 0.5f);
    if (cents > 99) cents = 99;
    const uint8_t ch = channel & 0x0F;
    out.push(MidiEvent::cc(ch, 101, 0));
    out.push(MidiEvent::cc(ch, 100, 0));
    out.push(MidiEvent::cc(ch, 6, whole));
    out.push(MidiEvent::cc(ch, 38, static_cast<uint8_t>(cents)));
    return out;
}

/// Zone layout: one Configuration Message for each zone, lower first. A zone
/// with no members is sent with a count of zero so a device that had it
/// enabled turns it off.
inline MpeMessageList<6> mpe_zone_layout_messages(const MpeConfig& config) {
    MpeMessageList<6> out;
    for (const auto& e : mpe_configuration_message(0, config.lower_zone.member_channels)) out.push(e);
    const uint8_t upper = config.upper_zone.manager_channel == 15 ? config.upper_zone.member_channels
                                                                  : uint8_t{0};
    for (const auto& e : mpe_configuration_message(15, upper)) out.push(e);
    return out;
}

/// Pitch-bend ranges for a whole zone: the manager channel gets
/// `manager_semitones` and every member channel gets `member_semitones`.
/// Send after the zone layout, because a Configuration Message resets ranges.
inline MpeMessageList<64> mpe_zone_pitch_bend_range_messages(const MpeZone& zone,
                                                              float manager_semitones,
                                                              float member_semitones) {
    MpeMessageList<64> out;
    if (zone.member_channels == 0) return out;
    for (const auto& e : mpe_pitch_bend_range_message(zone.manager_channel, manager_semitones)) out.push(e);
    for (uint8_t c = 0; c < 16; ++c) {
        if (!zone.contains_channel(c)) continue;
        for (const auto& e : mpe_pitch_bend_range_message(c, member_semitones)) out.push(e);
    }
    return out;
}

/// Send every event in `list` to `output`, in order.
template <std::size_t N>
void send_mpe_messages(MidiOutput& output, const MpeMessageList<N>& list) {
    for (const auto& e : list) output.send(e);
}

} // namespace pulp::midi
