#pragma once

#include <pulp/midi/utility_contract.hpp>

namespace pulp::midi {

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
        if (!valid_spec(spec))
            return false;
        spec_ = spec;
        valid_ = true;
        return true;
    }
    constexpr void reset() noexcept {}

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
            utility_detail::emit(
                output, utility_detail::with_channel(event, spec_.output_channel[channel]), report);
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
                copied_sidecars = utility_detail::emit_ump(output.ump(), routed) && copied_sidecars;
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
        if (!valid_spec(spec))
            return false;
        spec_ = spec;
        valid_ = true;
        return true;
    }
    constexpr void reset() noexcept {}

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
            if ((event.is_note_on() || event.is_note_off()) &&
                (event.note() < spec_.lowest || event.note() > spec_.highest))
                continue;
            utility_detail::emit(output, event, report);
        }
        bool copied_sidecars = utility_detail::copy_sysex_sidecar(input, output);
        if (const auto* source = input.ump()) {
            for (const auto& event : *source) {
                if (utility_detail::is_note(event.packet) &&
                    (event.packet.note_number() < spec_.lowest ||
                     event.packet.note_number() > spec_.highest))
                    continue;
                copied_sidecars = utility_detail::emit_ump(output.ump(), event) && copied_sidecars;
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
        if (!valid_spec(spec))
            return false;
        spec_ = spec;
        valid_ = true;
        return true;
    }
    constexpr void reset() noexcept {}

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
            if (!event.is_note_on() && !event.is_note_off()) {
                utility_detail::emit(lower, event, report);
                utility_detail::emit(upper, event, report);
                continue;
            }
            const bool is_upper = event.note() > spec_.split_note ||
                                  (event.note() == spec_.split_note && spec_.split_note_is_upper);
            auto routed = utility_detail::with_channel(event, is_upper ? spec_.upper_channel
                                                                       : spec_.lower_channel);
            utility_detail::emit(is_upper ? upper : lower, routed, report);
        }
        bool copied_sidecars = utility_detail::copy_sysex_sidecar(input, lower);
        copied_sidecars = utility_detail::copy_sysex_sidecar(input, upper) && copied_sidecars;
        if (const auto* source = input.ump()) {
            for (const auto& event : *source) {
                if (!utility_detail::is_note(event.packet)) {
                    copied_sidecars =
                        utility_detail::emit_ump(lower.ump(), event) && copied_sidecars;
                    copied_sidecars =
                        utility_detail::emit_ump(upper.ump(), event) && copied_sidecars;
                    continue;
                }
                const bool is_upper =
                    event.packet.note_number() > spec_.split_note ||
                    (event.packet.note_number() == spec_.split_note && spec_.split_note_is_upper);
                auto routed = event;
                routed.packet = utility_detail::with_channel(
                    event.packet, is_upper ? spec_.upper_channel : spec_.lower_channel);
                copied_sidecars =
                    utility_detail::emit_ump(is_upper ? upper.ump() : lower.ump(), routed) &&
                    copied_sidecars;
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
};

} // namespace pulp::midi
