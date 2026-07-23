#include <pulp/playback/external_sync.hpp>

#include "external_sync_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pulp::playback {
namespace {

MtcFrameRate decode_frame_rate(std::uint8_t code) noexcept {
    switch (code & 0x03) {
    case 0:
        return MtcFrameRate::Fps24;
    case 1:
        return MtcFrameRate::Fps25;
    case 2:
        return MtcFrameRate::Fps2997Drop;
    default:
        return MtcFrameRate::Fps30;
    }
}

std::int64_t labelled_frame_count(const MtcTimecode& timecode) noexcept {
    const auto ratio = detail::frame_rate_ratio(timecode.frame_rate);
    const auto total_seconds =
        (static_cast<std::int64_t>(timecode.hours) * 60 + timecode.minutes) * 60 + timecode.seconds;
    auto frames = total_seconds * ratio.nominal + timecode.frames;
    if (timecode.frame_rate == MtcFrameRate::Fps2997Drop) {
        const auto total_minutes =
            static_cast<std::int64_t>(timecode.hours) * 60 + timecode.minutes;
        frames -= 2 * (total_minutes - total_minutes / 10);
    }
    return frames;
}

std::int64_t samples_to_frame_count(timebase::SamplePosition position,
                                    timebase::RationalRate sample_rate,
                                    MtcFrameRate frame_rate) noexcept {
    if (!sample_rate.valid() || position.value <= 0)
        return 0;
    const auto ratio = detail::frame_rate_ratio(frame_rate);
    const long double frames =
        static_cast<long double>(position.value) * sample_rate.denominator * ratio.numerator /
        (static_cast<long double>(sample_rate.numerator) * ratio.denominator);
    if (frames >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(std::floor(frames));
}

} // namespace

namespace detail {

FrameRateRatio frame_rate_ratio(MtcFrameRate rate) noexcept {
    switch (rate) {
    case MtcFrameRate::Fps24:
        return {24, 1, 24, 0};
    case MtcFrameRate::Fps25:
        return {25, 1, 25, 1};
    case MtcFrameRate::Fps2997Drop:
        return {30'000, 1'001, 30, 2};
    case MtcFrameRate::Fps30:
        return {30, 1, 30, 3};
    }
    return {30, 1, 30, 3};
}

bool valid_frame_rate(MtcFrameRate rate) noexcept {
    switch (rate) {
    case MtcFrameRate::Fps24:
    case MtcFrameRate::Fps25:
    case MtcFrameRate::Fps2997Drop:
    case MtcFrameRate::Fps30:
        return true;
    }
    return false;
}

MtcTimecode frame_count_to_timecode(std::int64_t frames, MtcFrameRate rate) noexcept {
    frames = std::max<std::int64_t>(0, frames);
    const auto ratio = frame_rate_ratio(rate);
    constexpr std::int64_t hours_per_day = 24;
    if (rate != MtcFrameRate::Fps2997Drop) {
        const auto frames_per_day =
            hours_per_day * 60 * 60 * static_cast<std::int64_t>(ratio.nominal);
        frames %= frames_per_day;
        MtcTimecode result;
        result.frame_rate = rate;
        result.hours = static_cast<std::uint8_t>(
            frames / (60 * 60 * static_cast<std::int64_t>(ratio.nominal)));
        frames %= 60 * 60 * static_cast<std::int64_t>(ratio.nominal);
        result.minutes =
            static_cast<std::uint8_t>(frames / (60 * static_cast<std::int64_t>(ratio.nominal)));
        frames %= 60 * static_cast<std::int64_t>(ratio.nominal);
        result.seconds = static_cast<std::uint8_t>(frames / ratio.nominal);
        result.frames = static_cast<std::uint8_t>(frames % ratio.nominal);
        return result;
    }

    constexpr std::int64_t frames_per_ten_minutes = 17'982;
    constexpr std::int64_t frames_per_minute = 1'798;
    constexpr std::int64_t frames_per_day = 2'589'408;
    frames %= frames_per_day;
    const auto ten_minute_blocks = frames / frames_per_ten_minutes;
    const auto within_block = frames % frames_per_ten_minutes;
    const auto extra_minutes =
        within_block < 1'800 ? 0 : 1 + (within_block - 1'800) / frames_per_minute;
    const auto total_minutes = ten_minute_blocks * 10 + extra_minutes;
    const auto dropped = 2 * (total_minutes - total_minutes / 10);
    const auto labelled = frames + dropped;

    MtcTimecode result;
    result.frame_rate = rate;
    result.hours = static_cast<std::uint8_t>(labelled / (30 * 60 * 60));
    const auto within_hour = labelled % (30 * 60 * 60);
    result.minutes = static_cast<std::uint8_t>(within_hour / (30 * 60));
    const auto within_minute = within_hour % (30 * 60);
    result.seconds = static_cast<std::uint8_t>(within_minute / 30);
    result.frames = static_cast<std::uint8_t>(within_minute % 30);
    return result;
}

} // namespace detail

bool valid_mtc_timecode(const MtcTimecode& timecode) noexcept {
    if (!detail::valid_frame_rate(timecode.frame_rate))
        return false;
    const auto ratio = detail::frame_rate_ratio(timecode.frame_rate);
    if (timecode.hours >= 24 || timecode.minutes >= 60 || timecode.seconds >= 60 ||
        timecode.frames >= ratio.nominal)
        return false;
    if (timecode.frame_rate == MtcFrameRate::Fps2997Drop && timecode.seconds == 0 &&
        timecode.minutes % 10 != 0 && timecode.frames < 2)
        return false;
    return true;
}

timebase::SamplePosition mtc_timecode_to_samples(const MtcTimecode& timecode,
                                                 timebase::RationalRate sample_rate) noexcept {
    if (!sample_rate.valid() || !valid_mtc_timecode(timecode))
        return {};
    const auto ratio = detail::frame_rate_ratio(timecode.frame_rate);
    const long double samples =
        static_cast<long double>(labelled_frame_count(timecode)) * sample_rate.numerator *
        ratio.denominator / (static_cast<long double>(sample_rate.denominator) * ratio.numerator);
    if (samples >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return {std::numeric_limits<std::int64_t>::max()};
    // A timecode label denotes the first sample belonging to that video
    // frame. Fractional boundaries therefore round up; rounding to nearest
    // can place 29.97-drop labels one sample inside the preceding frame.
    return {static_cast<std::int64_t>(std::ceil(samples))};
}

MtcTimecode samples_to_mtc_timecode(timebase::SamplePosition position,
                                    timebase::RationalRate sample_rate,
                                    MtcFrameRate frame_rate) noexcept {
    return detail::frame_count_to_timecode(
        samples_to_frame_count(position, sample_rate, frame_rate), frame_rate);
}

MtcChaseUpdate MtcChaser::consume(const midi::MidiEvent& event,
                                  timebase::RationalRate sample_rate) noexcept {
    if (event.size() != 2 || event.data()[0] != 0xf1)
        return {};
    if (!sample_rate.valid())
        return {MtcChaseCode::Invalid};

    const auto data = event.data()[1];
    const auto piece = static_cast<std::uint8_t>((data >> 4) & 0x07);
    if (piece == 7 && (data & 0x08) != 0) {
        reset();
        return {MtcChaseCode::Invalid};
    }
    MtcDirection direction = MtcDirection::Unknown;
    if (have_last_piece_) {
        if (piece == static_cast<std::uint8_t>((last_piece_ + 1) & 0x07))
            direction = MtcDirection::Forward;
        else if (piece == static_cast<std::uint8_t>((last_piece_ + 7) & 0x07))
            direction = MtcDirection::Reverse;
        else {
            received_mask_ = 0;
            pieces_[piece] = data & 0x0f;
            received_mask_ = static_cast<std::uint8_t>(1u << piece);
            last_piece_ = piece;
            return {MtcChaseCode::Discontinuity};
        }
    }

    pieces_[piece] = data & 0x0f;
    received_mask_ |= static_cast<std::uint8_t>(1u << piece);
    last_piece_ = piece;
    have_last_piece_ = true;
    const bool cycle_complete =
        received_mask_ == 0xff && ((direction == MtcDirection::Forward && piece == 7) ||
                                   (direction == MtcDirection::Reverse && piece == 0));
    if (!cycle_complete)
        return {MtcChaseCode::Incomplete, direction};
    return complete(sample_rate, direction);
}

MtcChaseUpdate MtcChaser::consume_sysex(std::span<const std::uint8_t> message,
                                        timebase::RationalRate sample_rate) noexcept {
    if (message.size() != 10 || message[0] != 0xf0 || message[1] != 0x7f || message[3] != 0x01 ||
        message[4] != 0x01 || message[9] != 0xf7)
        return {};
    if (!sample_rate.valid())
        return {MtcChaseCode::Invalid};

    MtcTimecode timecode;
    timecode.frame_rate = decode_frame_rate(message[5] >> 5);
    timecode.hours = message[5] & 0x1f;
    timecode.minutes = message[6] & 0x7f;
    timecode.seconds = message[7] & 0x7f;
    timecode.frames = message[8] & 0x7f;
    if (!valid_mtc_timecode(timecode))
        return {MtcChaseCode::Invalid};
    reset();
    return {MtcChaseCode::Locked, MtcDirection::Unknown, timecode,
            mtc_timecode_to_samples(timecode, sample_rate)};
}

void MtcChaser::reset() noexcept {
    pieces_ = {};
    received_mask_ = 0;
    last_piece_ = 0;
    have_last_piece_ = false;
}

MtcChaseUpdate MtcChaser::complete(timebase::RationalRate sample_rate,
                                   MtcDirection direction) noexcept {
    MtcTimecode timecode;
    timecode.frames = static_cast<std::uint8_t>(pieces_[0] | (pieces_[1] << 4));
    timecode.seconds = static_cast<std::uint8_t>(pieces_[2] | (pieces_[3] << 4));
    timecode.minutes = static_cast<std::uint8_t>(pieces_[4] | (pieces_[5] << 4));
    timecode.hours = static_cast<std::uint8_t>(pieces_[6] | ((pieces_[7] & 0x01) << 4));
    timecode.frame_rate = decode_frame_rate(pieces_[7] >> 1);
    received_mask_ = 0;
    if (!valid_mtc_timecode(timecode))
        return {MtcChaseCode::Invalid, direction};
    return {MtcChaseCode::Locked, direction, timecode,
            mtc_timecode_to_samples(timecode, sample_rate)};
}

} // namespace pulp::playback
