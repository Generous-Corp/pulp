#pragma once

#include <pulp/midi/utility_contract.hpp>

namespace pulp::midi {

namespace routing_detail {

class HeldNoteLedger {
  public:
    constexpr bool empty() const noexcept { return total_ == 0; }
    constexpr void reset() noexcept {
        counts_.fill(0);
        total_ = 0;
    }

    bool can_forward(const MidiEvent& event) const noexcept {
        return !is_attack(event) ||
               counts_[utility_detail::key_index(event.channel(), event.note())] !=
                   std::numeric_limits<std::uint16_t>::max();
    }
    bool can_forward(const UmpPacket& packet) const noexcept {
        return !is_attack(packet) ||
               counts_[utility_detail::key_index(packet.channel(), packet.note_number())] !=
                   std::numeric_limits<std::uint16_t>::max();
    }

    void record(const MidiEvent& event, bool forwarded) noexcept {
        if (!forwarded) return;
        update(event.channel(), event.note(), is_attack(event), is_release(event));
    }
    void record(const UmpPacket& packet, bool forwarded) noexcept {
        if (!forwarded) return;
        update(packet.channel(), packet.note_number(), is_attack(packet), is_release(packet));
    }

  private:
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
    void update(std::uint8_t channel, std::uint8_t note, bool attack,
                bool release) noexcept {
        auto& count = counts_[utility_detail::key_index(channel, note)];
        if (attack) {
            ++count;
            ++total_;
        } else if (release && count != 0) {
            --count;
            --total_;
        }
    }

    std::array<std::uint16_t, 16 * 128> counts_{};
    std::size_t total_ = 0;
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
        for (const auto& event : input) {
            if (!utility_detail::is_channel_voice(event)) {
                utility_detail::emit(output, event, report);
                continue;
            }
            const auto channel = event.channel();
            if ((spec_.accepted_channels & (std::uint16_t{1} << channel)) == 0)
                continue;
            if (!held_notes_.can_forward(event)) {
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
                auto routed = event;
                routed.packet =
                    utility_detail::with_channel(event.packet, spec_.output_channel[channel]);
                if (!held_notes_.can_forward(event.packet)) {
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
        for (const auto& event : input) {
            if (utility_detail::is_note_addressed(event) &&
                (event.note() < spec_.lowest || event.note() > spec_.highest))
                continue;
            if (!held_notes_.can_forward(event)) {
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
                if (!held_notes_.can_forward(event.packet)) {
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
                if (!held_notes_.can_forward(event)) {
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
                    if (!held_notes_.can_forward(event.packet)) {
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
