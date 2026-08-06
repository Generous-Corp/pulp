#pragma once

#include <pulp/midi/utility_contract.hpp>

namespace pulp::midi {

namespace routing_detail {

inline UmpPacket note_off_packet(bool midi2, std::uint8_t group,
                                 std::uint8_t channel, std::uint8_t note) noexcept {
    if (midi2) return UmpPacket::note_off_2(group, channel, note);
    UmpPacket packet;
    packet.word_count = 1;
    packet.words[0] = (std::uint32_t{0x2} << 28) |
                      (static_cast<std::uint32_t>(group & 0x0f) << 24) |
                      (static_cast<std::uint32_t>(0x80 | (channel & 0x0f)) << 16) |
                      (static_cast<std::uint32_t>(note & 0x7f) << 8);
    return packet;
}

class HeldNoteLedger {
  public:
    constexpr bool empty() const noexcept { return total_ == 0; }
    constexpr void reset() noexcept {}

    bool can_forward(const MidiEvent& event) const noexcept {
        return !is_attack(event) ||
               midi_counts_[utility_detail::key_index(event.channel(), event.note())] !=
                   std::numeric_limits<std::uint8_t>::max();
    }
    bool can_forward(const UmpPacket& packet) const noexcept {
        return !is_attack(packet) ||
               ump_counts_[ump_key(packet)] !=
                   std::numeric_limits<std::uint8_t>::max();
    }

    bool consume_suppressed_release(const MidiEvent& event) noexcept {
        if (!is_release(event)) return false;
        return consume_suppressed(
            midi_suppressed_[utility_detail::key_index(event.channel(), event.note())]);
    }
    bool consume_suppressed_release(const UmpPacket& packet) noexcept {
        if (!is_release(packet)) return false;
        return consume_suppressed(ump_suppressed_[ump_key(packet)]);
    }

    void record(const MidiEvent& event, bool forwarded) noexcept {
        auto& count = midi_counts_[utility_detail::key_index(event.channel(), event.note())];
        auto& suppressed = midi_suppressed_[utility_detail::key_index(
            event.channel(), event.note())];
        auto& debt = midi_release_debt_[utility_detail::key_index(
            event.channel(), event.note())];
        update(count, suppressed, debt, midi_debt_total_, is_attack(event),
               is_release(event), forwarded);
    }
    void record(const UmpPacket& packet, bool forwarded) noexcept {
        auto& count = ump_counts_[ump_key(packet)];
        auto& suppressed = ump_suppressed_[ump_key(packet)];
        auto& debt = ump_release_debt_[ump_key(packet)];
        update(count, suppressed, debt, ump_debt_total_, is_attack(packet),
               is_release(packet), forwarded);
    }

    void retain_unprocessed_ownership(const MidiBuffer& input) noexcept {
        for (const auto& event : input) {
            if (!consume_suppressed_release(event)) record(event, false);
        }
        if (const auto* ump = input.ump())
            for (const auto& event : *ump) {
                if (!consume_suppressed_release(event.packet))
                    record(event.packet, false);
            }
    }

    template <typename Emit>
    bool drain_midi_releases(Emit&& emit) const noexcept {
        if (midi_debt_total_ == 0) return true;
        constexpr std::size_t kAttemptsPerBlock = 32;
        std::size_t attempts = 0;
        std::size_t visited = 0;
        while (visited < midi_release_debt_.size() && attempts < kAttemptsPerBlock) {
            const auto key = midi_drain_cursor_;
            midi_drain_cursor_ = (midi_drain_cursor_ + 1) % midi_release_debt_.size();
            ++visited;
            auto& debt = midi_release_debt_[key];
            if (debt == 0) continue;
            ++attempts;
            const auto channel = static_cast<std::uint8_t>(key / 128);
            const auto note = static_cast<std::uint8_t>(key % 128);
            if (emit(channel, note)) {
                --debt;
                --midi_counts_[key];
                --midi_debt_total_;
                --total_;
            }
        }
        return midi_debt_total_ == 0;
    }

    template <typename Emit>
    bool drain_ump_releases(Emit&& emit) const noexcept {
        if (ump_debt_total_ == 0) return true;
        constexpr std::size_t kAttemptsPerBlock = 32;
        std::size_t attempts = 0;
        std::size_t visited = 0;
        while (visited < ump_release_debt_.size() && attempts < kAttemptsPerBlock) {
            const auto key = ump_drain_cursor_;
            ump_drain_cursor_ = (ump_drain_cursor_ + 1) % ump_release_debt_.size();
            ++visited;
            auto& debt = ump_release_debt_[key];
            if (debt == 0) continue;
            ++attempts;
            constexpr std::size_t kKeysPerProtocol = 16 * 16 * 128;
            const bool midi2 = key >= kKeysPerProtocol;
            const auto protocol_key = key % kKeysPerProtocol;
            const auto group = static_cast<std::uint8_t>(protocol_key / (16 * 128));
            const auto remainder = protocol_key % (16 * 128);
            const auto channel = static_cast<std::uint8_t>(remainder / 128);
            const auto note = static_cast<std::uint8_t>(remainder % 128);
            if (emit(midi2, group, channel, note)) {
                --debt;
                --ump_counts_[key];
                --ump_debt_total_;
                --total_;
            }
        }
        return ump_debt_total_ == 0;
    }

  private:
    static std::size_t ump_key(const UmpPacket& packet) noexcept {
        constexpr std::size_t kKeysPerProtocol = 16 * 16 * 128;
        const std::size_t protocol =
            packet.message_type() == UmpMessageType::Midi2ChannelVoice ? 1 : 0;
        return protocol * kKeysPerProtocol +
               (static_cast<std::size_t>(packet.group()) * 16 + packet.channel()) * 128 +
               packet.note_number();
    }
    static bool is_attack(const MidiEvent& event) noexcept {
        return event.is_note_on() && event.velocity() != 0;
    }
    static bool is_release(const MidiEvent& event) noexcept {
        return event.is_note_off() || (event.is_note_on() && event.velocity() == 0);
    }
    static bool is_attack(const UmpPacket& packet) noexcept {
        if (!utility_detail::is_channel_voice(packet)) return false;
        const auto status = static_cast<std::uint8_t>(packet.status() & 0xf0);
        if (status != 0x90) return false;
        return packet.message_type() != UmpMessageType::Midi1ChannelVoice ||
               (packet.words[0] & 0x7f) != 0;
    }
    static bool is_release(const UmpPacket& packet) noexcept {
        if (!utility_detail::is_channel_voice(packet)) return false;
        const auto status = static_cast<std::uint8_t>(packet.status() & 0xf0);
        return status == 0x80 ||
               (status == 0x90 && packet.message_type() == UmpMessageType::Midi1ChannelVoice &&
                (packet.words[0] & 0x7f) == 0);
    }
    bool consume_suppressed(std::uint8_t& suppressed) noexcept {
        if (suppressed == 0) return false;
        --suppressed;
        --total_;
        return true;
    }
    void update(std::uint8_t& count, std::uint8_t& suppressed,
                std::uint8_t& debt,
                std::size_t& debt_total, bool attack, bool release,
                bool forwarded) noexcept {
        if (attack && forwarded) {
            ++count;
            ++total_;
        } else if (attack && !forwarded &&
                   suppressed != std::numeric_limits<std::uint8_t>::max()) {
            ++suppressed;
            ++total_;
        } else if (release && forwarded && count != 0) {
            --count;
            --total_;
        } else if (release && !forwarded && debt < count) {
            ++debt;
            ++debt_total;
        }
    }

    mutable std::array<std::uint8_t, 16 * 128> midi_counts_{};
    mutable std::array<std::uint8_t, 16 * 128> midi_suppressed_{};
    mutable std::array<std::uint8_t, 16 * 128> midi_release_debt_{};
    mutable std::array<std::uint8_t, 2 * 16 * 16 * 128> ump_counts_{};
    mutable std::array<std::uint8_t, 2 * 16 * 16 * 128> ump_suppressed_{};
    mutable std::array<std::uint8_t, 2 * 16 * 16 * 128> ump_release_debt_{};
    mutable std::size_t midi_drain_cursor_ = 0;
    mutable std::size_t ump_drain_cursor_ = 0;
    mutable std::size_t midi_debt_total_ = 0;
    mutable std::size_t ump_debt_total_ = 0;
    mutable std::size_t total_ = 0;
};

} // namespace routing_detail

struct ChannelRouteSpec {
    std::uint16_t accepted_channels = 0xffff;
    std::array<std::uint8_t, 16> output_channel{0, 1, 2,  3,  4,  5,  6,  7,
                                                8, 9, 10, 11, 12, 13, 14, 15};
};

class ChannelRouter {
  public:
    static constexpr MidiUtilityContract contract() noexcept {
        return {1, 0, MidiUtilityOverflowPolicy::DropUnstarted,
                MidiUtilitySameSampleOrder::InputStable, MidiUtilityTransportRequirement::None};
    }

    static constexpr bool valid_spec(const ChannelRouteSpec& spec) noexcept {
        for (const auto channel : spec.output_channel)
            if (channel > 15)
                return false;
        return true;
    }
    explicit constexpr ChannelRouter(ChannelRouteSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}
    constexpr bool valid() const noexcept {
        return valid_;
    }
    constexpr bool replace_spec(ChannelRouteSpec spec) noexcept {
        if (!valid_spec(spec) || !held_notes_.empty())
            return false;
        spec_ = spec;
        valid_ = true;
        return true;
    }
    constexpr void reset() noexcept { held_notes_.reset(); }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output) const {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output))
            return {0, input.size(), 0, false};
        if (!valid_)
            return {0, input.size(), 0, false};
        const bool midi_drained = held_notes_.drain_midi_releases(
            [&](std::uint8_t channel, std::uint8_t note) {
                return utility_detail::emit(
                    output,
                    utility_detail::with_channel(
                        MidiEvent::note_off(channel, note), spec_.output_channel[channel]),
                    report);
            });
        const bool ump_drained = held_notes_.drain_ump_releases(
            [&](bool midi2, std::uint8_t group, std::uint8_t channel,
                std::uint8_t note) {
                const bool emitted = utility_detail::emit_ump(
                    output.ump(),
                    {routing_detail::note_off_packet(
                         midi2, group, spec_.output_channel[channel], note),
                     0});
                if (!emitted) {
                    ++report.dropped;
                    report.complete = false;
                }
                return emitted;
            });
        if (!midi_drained || !ump_drained) {
            held_notes_.retain_unprocessed_ownership(input);
            report.dropped += input.size();
            report.complete = false;
            return report;
        }
        for (const auto& event : input) {
            if (!utility_detail::is_channel_voice(event)) {
                utility_detail::emit(output, event, report);
                continue;
            }
            const auto channel = event.channel();
            if ((spec_.accepted_channels & (std::uint16_t{1} << channel)) == 0)
                continue;
            if (held_notes_.consume_suppressed_release(event))
                continue;
            if (!held_notes_.can_forward(event)) {
                held_notes_.record(event, false);
                ++report.dropped;
                report.complete = false;
                continue;
            }
            const bool forwarded = utility_detail::emit(
                output, utility_detail::with_channel(event, spec_.output_channel[channel]), report);
            held_notes_.record(event, forwarded);
        }
        bool copied_sidecars = utility_detail::copy_sysex_sidecar(input, output);
        if (const auto* source = input.ump()) {
            for (const auto& event : *source) {
                if (!utility_detail::is_channel_voice(event.packet)) {
                    copied_sidecars =
                        utility_detail::emit_ump(output.ump(), event) && copied_sidecars;
                    continue;
                }
                const auto channel = event.packet.channel();
                if ((spec_.accepted_channels & (std::uint16_t{1} << channel)) == 0)
                    continue;
                if (held_notes_.consume_suppressed_release(event.packet))
                    continue;
                auto routed = event;
                routed.packet =
                    utility_detail::with_channel(event.packet, spec_.output_channel[channel]);
                if (!held_notes_.can_forward(event.packet)) {
                    held_notes_.record(event.packet, false);
                    ++report.dropped;
                    report.complete = false;
                    continue;
                }
                const bool forwarded = utility_detail::emit_ump(output.ump(), routed);
                held_notes_.record(event.packet, forwarded);
                copied_sidecars = forwarded && copied_sidecars;
            }
        }
        if (!copied_sidecars) {
            ++report.dropped;
            report.complete = false;
        }
        return report;
    }

  private:
    ChannelRouteSpec spec_{};
    bool valid_ = true;
    mutable routing_detail::HeldNoteLedger held_notes_{};
};

struct NoteRangeSpec {
    std::uint8_t lowest = 0;
    std::uint8_t highest = 127;
};

class NoteRangeFilter {
  public:
    static constexpr MidiUtilityContract contract() noexcept {
        return {1, 0, MidiUtilityOverflowPolicy::DropUnstarted,
                MidiUtilitySameSampleOrder::InputStable, MidiUtilityTransportRequirement::None};
    }

    static constexpr bool valid_spec(NoteRangeSpec spec) noexcept {
        return spec.lowest <= 127 && spec.highest <= 127 && spec.lowest <= spec.highest;
    }
    explicit constexpr NoteRangeFilter(NoteRangeSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}
    constexpr bool valid() const noexcept {
        return valid_;
    }
    constexpr bool replace_spec(NoteRangeSpec spec) noexcept {
        if (!valid_spec(spec) || !held_notes_.empty())
            return false;
        spec_ = spec;
        valid_ = true;
        return true;
    }
    constexpr void reset() noexcept { held_notes_.reset(); }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output) const {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output))
            return {0, input.size(), 0, false};
        if (!valid_)
            return {0, input.size(), 0, false};
        const bool midi_drained = held_notes_.drain_midi_releases(
            [&](std::uint8_t channel, std::uint8_t note) {
                return utility_detail::emit(
                    output, MidiEvent::note_off(channel, note), report);
            });
        const bool ump_drained = held_notes_.drain_ump_releases(
            [&](bool midi2, std::uint8_t group, std::uint8_t channel,
                std::uint8_t note) {
                const bool emitted = utility_detail::emit_ump(
                    output.ump(),
                    {routing_detail::note_off_packet(midi2, group, channel, note), 0});
                if (!emitted) {
                    ++report.dropped;
                    report.complete = false;
                }
                return emitted;
            });
        if (!midi_drained || !ump_drained) {
            held_notes_.retain_unprocessed_ownership(input);
            report.dropped += input.size();
            report.complete = false;
            return report;
        }
        for (const auto& event : input) {
            if (utility_detail::is_note_addressed(event) &&
                (event.note() < spec_.lowest || event.note() > spec_.highest))
                continue;
            if (held_notes_.consume_suppressed_release(event))
                continue;
            if (!held_notes_.can_forward(event)) {
                held_notes_.record(event, false);
                ++report.dropped;
                report.complete = false;
                continue;
            }
            const bool forwarded = utility_detail::emit(output, event, report);
            held_notes_.record(event, forwarded);
        }
        bool copied_sidecars = utility_detail::copy_sysex_sidecar(input, output);
        if (const auto* source = input.ump()) {
            for (const auto& event : *source) {
                if (utility_detail::is_note_addressed(event.packet) &&
                    (event.packet.note_number() < spec_.lowest ||
                     event.packet.note_number() > spec_.highest))
                    continue;
                if (held_notes_.consume_suppressed_release(event.packet))
                    continue;
                if (!held_notes_.can_forward(event.packet)) {
                    held_notes_.record(event.packet, false);
                    ++report.dropped;
                    report.complete = false;
                    continue;
                }
                const bool forwarded = utility_detail::emit_ump(output.ump(), event);
                held_notes_.record(event.packet, forwarded);
                copied_sidecars = forwarded && copied_sidecars;
            }
        }
        if (!copied_sidecars) {
            ++report.dropped;
            report.complete = false;
        }
        return report;
    }

  private:
    NoteRangeSpec spec_{};
    bool valid_ = true;
    mutable routing_detail::HeldNoteLedger held_notes_{};
};

struct KeyboardSplitSpec {
    std::uint8_t split_note = 60;
    bool split_note_is_upper = true;
    std::uint8_t lower_channel = 0;
    std::uint8_t upper_channel = 1;
};

class KeyboardSplit {
  public:
    static constexpr MidiUtilityContract contract() noexcept {
        return {2, 0, MidiUtilityOverflowPolicy::DropUnstarted,
                MidiUtilitySameSampleOrder::InputStable, MidiUtilityTransportRequirement::None};
    }

    static constexpr bool valid_spec(KeyboardSplitSpec spec) noexcept {
        return spec.split_note <= 127 && spec.lower_channel <= 15 && spec.upper_channel <= 15;
    }
    explicit constexpr KeyboardSplit(KeyboardSplitSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}
    constexpr bool valid() const noexcept {
        return valid_;
    }
    constexpr bool replace_spec(KeyboardSplitSpec spec) noexcept {
        if (!valid_spec(spec) || !held_notes_.empty())
            return false;
        spec_ = spec;
        valid_ = true;
        return true;
    }
    constexpr void reset() noexcept { held_notes_.reset(); }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& lower,
                                     MidiBuffer& upper) const {
        if (utility_detail::blocks_alias(input, lower) ||
            utility_detail::blocks_alias(input, upper) ||
            utility_detail::blocks_alias(lower, upper))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(lower);
        utility_detail::clear_output(upper);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(lower) || !utility_detail::ready(upper))
            return {0, input.size(), 0, false};
        if (!valid_)
            return {0, input.size(), 0, false};
        const bool midi_drained = held_notes_.drain_midi_releases(
            [&](std::uint8_t channel, std::uint8_t note) {
                const bool is_upper =
                    note > spec_.split_note ||
                    (note == spec_.split_note && spec_.split_note_is_upper);
                return utility_detail::emit(
                    is_upper ? upper : lower,
                    MidiEvent::note_off(
                        is_upper ? spec_.upper_channel : spec_.lower_channel, note),
                    report);
            });
        const bool ump_drained = held_notes_.drain_ump_releases(
            [&](bool midi2, std::uint8_t group, std::uint8_t channel,
                std::uint8_t note) {
                (void)channel;
                const bool is_upper =
                    note > spec_.split_note ||
                    (note == spec_.split_note && spec_.split_note_is_upper);
                const bool emitted = utility_detail::emit_ump(
                    is_upper ? upper.ump() : lower.ump(),
                    {routing_detail::note_off_packet(
                         midi2, group,
                         is_upper ? spec_.upper_channel : spec_.lower_channel, note),
                     0});
                if (!emitted) {
                    ++report.dropped;
                    report.complete = false;
                }
                return emitted;
            });
        if (!midi_drained || !ump_drained) {
            held_notes_.retain_unprocessed_ownership(input);
            report.dropped += input.size();
            report.complete = false;
            return report;
        }
        for (const auto& event : input) {
            if (!utility_detail::is_channel_voice(event)) {
                utility_detail::emit(lower, event, report);
                utility_detail::emit(upper, event, report);
                continue;
            }
            if (utility_detail::is_note_addressed(event)) {
                const bool is_upper =
                    event.note() > spec_.split_note ||
                    (event.note() == spec_.split_note && spec_.split_note_is_upper);
                auto routed = utility_detail::with_channel(event, is_upper ? spec_.upper_channel
                                                                           : spec_.lower_channel);
                if (held_notes_.consume_suppressed_release(event))
                    continue;
                if (!held_notes_.can_forward(event)) {
                    held_notes_.record(event, false);
                    ++report.dropped;
                    report.complete = false;
                    continue;
                }
                const bool forwarded = utility_detail::emit(
                    is_upper ? upper : lower, routed, report);
                held_notes_.record(event, forwarded);
                continue;
            }
            utility_detail::emit(lower, utility_detail::with_channel(event, spec_.lower_channel),
                                 report);
            utility_detail::emit(upper, utility_detail::with_channel(event, spec_.upper_channel),
                                 report);
        }
        bool copied_sidecars = utility_detail::copy_sysex_sidecar(input, lower);
        copied_sidecars = utility_detail::copy_sysex_sidecar(input, upper) && copied_sidecars;
        if (const auto* source = input.ump()) {
            for (const auto& event : *source) {
                if (!utility_detail::is_channel_voice(event.packet)) {
                    copied_sidecars =
                        utility_detail::emit_ump(lower.ump(), event) && copied_sidecars;
                    copied_sidecars =
                        utility_detail::emit_ump(upper.ump(), event) && copied_sidecars;
                    continue;
                }
                if (utility_detail::is_note_addressed(event.packet)) {
                    const bool is_upper = event.packet.note_number() > spec_.split_note ||
                                          (event.packet.note_number() == spec_.split_note &&
                                           spec_.split_note_is_upper);
                    auto routed = event;
                    routed.packet = utility_detail::with_channel(
                        event.packet, is_upper ? spec_.upper_channel : spec_.lower_channel);
                    if (held_notes_.consume_suppressed_release(event.packet))
                        continue;
                    if (!held_notes_.can_forward(event.packet)) {
                        held_notes_.record(event.packet, false);
                        ++report.dropped;
                        report.complete = false;
                        continue;
                    }
                    const bool forwarded = utility_detail::emit_ump(
                        is_upper ? upper.ump() : lower.ump(), routed);
                    held_notes_.record(event.packet, forwarded);
                    copied_sidecars = forwarded && copied_sidecars;
                    continue;
                }
                auto lower_event = event;
                lower_event.packet =
                    utility_detail::with_channel(event.packet, spec_.lower_channel);
                copied_sidecars =
                    utility_detail::emit_ump(lower.ump(), lower_event) && copied_sidecars;
                auto upper_event = event;
                upper_event.packet =
                    utility_detail::with_channel(event.packet, spec_.upper_channel);
                copied_sidecars =
                    utility_detail::emit_ump(upper.ump(), upper_event) && copied_sidecars;
            }
        }
        if (!copied_sidecars) {
            ++report.dropped;
            report.complete = false;
        }
        return report;
    }

  private:
    KeyboardSplitSpec spec_{};
    bool valid_ = true;
    mutable routing_detail::HeldNoteLedger held_notes_{};
};

} // namespace pulp::midi
