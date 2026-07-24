#include <pulp/playback/external_sync.hpp>

#include "external_sync_internal.hpp"

#include <pulp/playback/transport.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/timebase/tick.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace pulp::playback {
namespace {

std::int64_t ceil_div(std::int64_t value, std::int64_t divisor) noexcept {
    if (divisor <= 0)
        return 0;
    auto quotient = value / divisor;
    const auto remainder = value % divisor;
    if (remainder > 0)
        ++quotient;
    return quotient;
}

bool first_multiple_index_at_or_after(long double value, std::int64_t interval,
                                      std::int64_t& result) noexcept {
    if (interval <= 0 || !std::isfinite(value))
        return false;
    const auto minimum = ceil_div(std::numeric_limits<std::int64_t>::min(), interval);
    const auto maximum = std::numeric_limits<std::int64_t>::max() / interval;
    const auto candidate = std::ceil(value / static_cast<long double>(interval));
    if (candidate > static_cast<long double>(maximum))
        return false;
    result = candidate < static_cast<long double>(minimum)
                 ? minimum
                 : static_cast<std::int64_t>(candidate);
    return true;
}

std::int64_t saturating_add(std::int64_t value, std::uint32_t increment) noexcept {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (value > maximum - static_cast<std::int64_t>(increment))
        return maximum;
    return value + static_cast<std::int64_t>(increment);
}

bool checked_multiply(std::int64_t value, std::int64_t multiplier, std::int64_t& result) noexcept {
    if (value > 0 && value > std::numeric_limits<std::int64_t>::max() / multiplier)
        return false;
    if (value < 0 && value < std::numeric_limits<std::int64_t>::min() / multiplier)
        return false;
    result = value * multiplier;
    return true;
}

timebase::SamplePosition quarter_frame_sample(std::int64_t quarter,
                                              timebase::RationalRate sample_rate,
                                              MtcFrameRate frame_rate) noexcept {
    if (quarter <= 0)
        return {};
    const auto ratio = detail::frame_rate_ratio(frame_rate);
    const long double sample =
        static_cast<long double>(quarter) * sample_rate.numerator * ratio.denominator /
        (static_cast<long double>(sample_rate.denominator) * ratio.numerator * 4.0L);
    if (sample >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return {std::numeric_limits<std::int64_t>::max()};
    return {static_cast<std::int64_t>(std::ceil(sample))};
}

std::int64_t first_quarter_at_or_after(timebase::SamplePosition sample,
                                       timebase::RationalRate sample_rate,
                                       MtcFrameRate frame_rate) noexcept {
    if (sample.value <= 0)
        return 0;
    const auto ratio = detail::frame_rate_ratio(frame_rate);
    const long double quarter =
        static_cast<long double>(sample.value) * sample_rate.denominator * ratio.numerator * 4.0L /
        (static_cast<long double>(sample_rate.numerator) * ratio.denominator);
    auto candidate = static_cast<std::int64_t>(std::floor(quarter));
    while (quarter_frame_sample(candidate, sample_rate, frame_rate) < sample)
        ++candidate;
    while (candidate > 0 && quarter_frame_sample(candidate - 1, sample_rate, frame_rate) >= sample)
        --candidate;
    return candidate;
}

std::uint8_t quarter_frame_data(std::uint8_t piece, const MtcTimecode& timecode) noexcept {
    std::uint8_t nibble = 0;
    switch (piece) {
    case 0:
        nibble = timecode.frames & 0x0f;
        break;
    case 1:
        nibble = (timecode.frames >> 4) & 0x01;
        break;
    case 2:
        nibble = timecode.seconds & 0x0f;
        break;
    case 3:
        nibble = (timecode.seconds >> 4) & 0x03;
        break;
    case 4:
        nibble = timecode.minutes & 0x0f;
        break;
    case 5:
        nibble = (timecode.minutes >> 4) & 0x03;
        break;
    case 6:
        nibble = timecode.hours & 0x0f;
        break;
    case 7:
        nibble = static_cast<std::uint8_t>(
            ((timecode.hours >> 4) & 0x01) |
            (detail::frame_rate_ratio(timecode.frame_rate).mtc_code << 1));
        break;
    }
    return static_cast<std::uint8_t>((piece << 4) | nibble);
}

bool reserve_message(ExternalSyncOutputResult& result, std::uint32_t maximum) noexcept {
    if (result.short_messages + result.sysex_messages < maximum)
        return true;
    result.code = ExternalSyncOutputCode::OutputOverflow;
    return false;
}

bool append_short(midi::MidiBuffer& output, std::uint8_t status, std::uint8_t data,
                  std::int32_t offset, std::uint32_t maximum,
                  ExternalSyncOutputResult& result) noexcept {
    if (!reserve_message(result, maximum))
        return false;
    midi::MidiEvent event{choc::midi::ShortMessage(status, data, 0), offset, 0.0};
    if (!output.add(std::move(event))) {
        result.code = ExternalSyncOutputCode::OutputOverflow;
        return false;
    }
    ++result.short_messages;
    return true;
}

bool append_short(midi::MidiBuffer& output, std::uint8_t status, std::uint8_t data1,
                  std::uint8_t data2, std::int32_t offset, std::uint32_t maximum,
                  ExternalSyncOutputResult& result) noexcept {
    if (!reserve_message(result, maximum))
        return false;
    midi::MidiEvent event{choc::midi::ShortMessage(status, data1, data2), offset, 0.0};
    if (!output.add(std::move(event))) {
        result.code = ExternalSyncOutputCode::OutputOverflow;
        return false;
    }
    ++result.short_messages;
    return true;
}

bool append_full_frame(midi::MidiBuffer& output, std::int32_t offset, const MtcTimecode& timecode,
                       std::uint8_t device_id, std::uint32_t maximum,
                       ExternalSyncOutputResult& result) noexcept {
    if (!reserve_message(result, maximum))
        return false;
    const auto ratio = detail::frame_rate_ratio(timecode.frame_rate);
    const std::array<std::uint8_t, 10> message{
        0xf0,
        0x7f,
        static_cast<std::uint8_t>(device_id & 0x7f),
        0x01,
        0x01,
        static_cast<std::uint8_t>((ratio.mtc_code << 5) | timecode.hours),
        timecode.minutes,
        timecode.seconds,
        timecode.frames,
        0xf7};
    if (!output.add_sysex_copy(message.data(), message.size(), offset)) {
        result.code = ExternalSyncOutputCode::OutputOverflow;
        return false;
    }
    ++result.sysex_messages;
    return true;
}

std::int32_t bounded_offset(std::int64_t value) noexcept {
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(value, 0, std::numeric_limits<std::int32_t>::max()));
}

bool append_song_position(midi::MidiBuffer& output, long double tick,
                          std::int32_t offset, std::uint32_t maximum,
                          ExternalSyncOutputResult& result) noexcept {
    const auto sixteenth = timebase::kTicksPerQuarter / 4;
    const auto raw_position = std::floor(tick / static_cast<long double>(sixteenth));
    const auto position = raw_position <= 0.0L
        ? std::int64_t{0}
        : raw_position >= 0x3fff
            ? std::int64_t{0x3fff}
            : static_cast<std::int64_t>(raw_position);
    return append_short(output, 0xf2, static_cast<std::uint8_t>(position & 0x7f),
                        static_cast<std::uint8_t>((position >> 7) & 0x7f), offset, maximum, result);
}

} // namespace

ExternalSyncOutputResult ExternalSyncOutput::process(const TransportSnapshot& transport,
                                                     midi::MidiBuffer& output) const noexcept {
    runtime::ScopedNoAlloc no_alloc_guard;
    ExternalSyncOutputResult result;
    if (!valid_transport_ranges(transport)) {
        result.code = ExternalSyncOutputCode::InvalidTransport;
        return result;
    }
    if (!transport.sample_rate.valid() ||
        transport.sample_rate.normalized() != transport.tempo_map->sample_rate().normalized()) {
        result.code = ExternalSyncOutputCode::InvalidSampleRate;
        return result;
    }
    if (config_.emit_mtc && !detail::valid_frame_rate(config_.mtc_frame_rate)) {
        result.code = ExternalSyncOutputCode::InvalidFrameRate;
        return result;
    }
    if (config_.max_messages_per_block == 0) {
        result.code = ExternalSyncOutputCode::InvalidOutputLimit;
        return result;
    }

    const auto& first_range = transport.ranges[0];
    const auto first_tick = first_range.timeline_tick_start;
    const auto first_position_tick =
        first_range.has_precise_host_ticks
            ? static_cast<long double>(first_range.host_tick_start)
            : static_cast<long double>(first_tick.value);
    if (config_.emit_midi_clock && transport.transport_started) {
        if (first_position_tick != 0.0L) {
            if (!append_song_position(output, first_position_tick, 0,
                                      config_.max_messages_per_block, result) ||
                !append_short(output, 0xfb, 0, 0, config_.max_messages_per_block, result))
                return result;
        } else {
            if (!append_short(output, 0xfa, 0, 0, config_.max_messages_per_block, result))
                return result;
        }
    } else if (config_.emit_midi_clock && transport.transport_changed && !transport.is_playing) {
        if (!append_short(output, 0xfc, 0, 0, config_.max_messages_per_block, result))
            return result;
    }

    for (std::uint8_t range_index = 0; range_index < transport.range_count; ++range_index) {
        const auto& range = transport.ranges[range_index];
        const auto range_end = timebase::SamplePosition{
            saturating_add(range.timeline_sample_start.value, range.frame_count)};
        if (config_.emit_midi_clock && range.discontinuity) {
            const auto offset = bounded_offset(range.sample_offset);
            const auto position_tick =
                range.has_precise_host_ticks
                    ? static_cast<long double>(range.host_tick_start)
                    : static_cast<long double>(range.timeline_tick_start.value);
            if (!append_song_position(output, position_tick, offset,
                                      config_.max_messages_per_block, result) ||
                (transport.is_playing &&
                 !append_short(output, 0xfb, 0, offset, config_.max_messages_per_block, result)))
                return result;
        }
        if (config_.emit_mtc && (transport.reset_requested || range.discontinuity ||
                                 (transport.transport_started && range_index == 0))) {
            if (!append_full_frame(output, bounded_offset(range.sample_offset),
                                   samples_to_mtc_timecode(range.timeline_sample_start,
                                                           transport.sample_rate,
                                                           config_.mtc_frame_rate),
                                   config_.mtc_device_id, config_.max_messages_per_block, result))
                return result;
        }
        if (!transport.is_playing)
            continue;

        if (config_.emit_midi_clock) {
            constexpr auto pulse_ticks = timebase::kTicksPerQuarter / 24;
            auto pulse = ceil_div(range.timeline_tick_start.value, pulse_ticks);
            const auto has_first_pulse =
                !range.has_precise_host_ticks ||
                first_multiple_index_at_or_after(
                    static_cast<long double>(range.host_tick_start), pulse_ticks, pulse);
            const auto tick_end =
                range.has_precise_host_ticks
                    ? static_cast<long double>(range.host_tick_end)
                    : static_cast<long double>(range.timeline_tick_end.value);
            while (has_first_pulse) {
                std::int64_t tick_value = 0;
                if (!checked_multiply(pulse, pulse_ticks, tick_value))
                    break;
                const timebase::TickPosition tick{tick_value};
                if (!(static_cast<long double>(tick.value) < tick_end))
                    break;
                std::uint32_t mapped_offset = 0;
                if (range.host_beat_mapping &&
                    host_mapped_output_offset_for_tick(range, tick, mapped_offset)) {
                    if (!append_short(output, 0xf8, 0,
                                      bounded_offset(range.sample_offset + mapped_offset),
                                      config_.max_messages_per_block, result))
                        return result;
                    if (pulse == std::numeric_limits<std::int64_t>::max())
                        break;
                    ++pulse;
                    continue;
                }
                if (range.has_precise_host_ticks) {
                    if (pulse == std::numeric_limits<std::int64_t>::max())
                        break;
                    ++pulse;
                    continue;
                }
                const auto sample = transport.tempo_map->ticks_to_samples(tick);
                if (sample >= range.timeline_sample_start && sample < range_end) {
                    if (!append_short(output, 0xf8, 0,
                                      bounded_offset(range.sample_offset + sample.value -
                                                     range.timeline_sample_start.value),
                                      config_.max_messages_per_block, result))
                        return result;
                }
                if (pulse == std::numeric_limits<std::int64_t>::max())
                    break;
                ++pulse;
            }
        }

        if (config_.emit_mtc && range_end.value > 0) {
            auto quarter = first_quarter_at_or_after(range.timeline_sample_start,
                                                     transport.sample_rate, config_.mtc_frame_rate);
            while (true) {
                const auto sample =
                    quarter_frame_sample(quarter, transport.sample_rate, config_.mtc_frame_rate);
                if (sample >= range_end)
                    break;
                if (sample >= range.timeline_sample_start) {
                    const auto piece = static_cast<std::uint8_t>(quarter & 0x07);
                    const auto cycle = quarter / 8;
                    const auto encoded_frame =
                        cycle > (std::numeric_limits<std::int64_t>::max() - 2) / 2
                            ? std::numeric_limits<std::int64_t>::max()
                            : cycle * 2 + 2;
                    const auto timecode =
                        detail::frame_count_to_timecode(encoded_frame, config_.mtc_frame_rate);
                    if (!append_short(output, 0xf1, quarter_frame_data(piece, timecode),
                                      bounded_offset(range.sample_offset + sample.value -
                                                     range.timeline_sample_start.value),
                                      config_.max_messages_per_block, result))
                        return result;
                }
                if (quarter == std::numeric_limits<std::int64_t>::max())
                    break;
                ++quarter;
            }
        }
    }
    return result;
}

} // namespace pulp::playback
