#include "smf_decode.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace pulp::timeline::detail {
namespace {

using runtime::Err;
using runtime::Ok;

using DecodeResult = runtime::Result<SmfDecodedFile, SmfError>;

constexpr std::size_t kChunkTypeBytes = 4;
constexpr std::size_t kMinimumHeaderPayload = 6;

constexpr std::uint8_t kMetaStatus = 0xffu;
constexpr std::uint8_t kSysExStatus = 0xf0u;
constexpr std::uint8_t kSysExEscapeStatus = 0xf7u;
constexpr std::uint8_t kMetaTrackName = 0x03u;
constexpr std::uint8_t kMetaEndOfTrack = 0x2fu;
constexpr std::uint8_t kMetaSetTempo = 0x51u;
constexpr std::uint8_t kMetaTimeSignature = 0x58u;

std::string byte_value(unsigned value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text = "0x";
    text.push_back(kDigits[(value >> 4u) & 0x0fu]);
    text.push_back(kDigits[value & 0x0fu]);
    return text;
}

// A bounds-checked cursor over the file bytes. Every accessor either returns a
// value fully contained in the span or std::nullopt; no caller can read past
// the end even when a declared length lies.
class ByteReader {
  public:
    explicit ByteReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    std::size_t remaining() const noexcept {
        return data_.size() - position_;
    }
    bool exhausted() const noexcept {
        return remaining() == 0;
    }

    std::optional<std::uint8_t> read_u8() noexcept {
        if (remaining() < 1)
            return std::nullopt;
        return data_[position_++];
    }

    std::optional<std::uint8_t> peek_u8() const noexcept {
        if (remaining() < 1)
            return std::nullopt;
        return data_[position_];
    }

    std::optional<std::uint16_t> read_u16() noexcept {
        if (remaining() < 2)
            return std::nullopt;
        const auto high = static_cast<std::uint16_t>(data_[position_]);
        const auto low = static_cast<std::uint16_t>(data_[position_ + 1]);
        position_ += 2;
        return static_cast<std::uint16_t>((high << 8u) | low);
    }

    std::optional<std::uint32_t> read_u32() noexcept {
        if (remaining() < 4)
            return std::nullopt;
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index)
            value = (value << 8u) | data_[position_ + index];
        position_ += 4;
        return value;
    }

    std::optional<std::span<const std::uint8_t>> read_bytes(std::size_t count) noexcept {
        if (remaining() < count)
            return std::nullopt;
        const auto slice = data_.subspan(position_, count);
        position_ += count;
        return slice;
    }

    // Variable-length quantity: up to four 7-bit groups, high bit as the
    // continuation flag. A fifth group is malformed, not merely large.
    std::optional<std::uint32_t> read_variable_length() noexcept {
        std::uint32_t value = 0;
        for (std::size_t group = 0; group < 4; ++group) {
            const auto byte = read_u8();
            if (!byte)
                return std::nullopt;
            value = (value << 7u) | static_cast<std::uint32_t>(*byte & 0x7fu);
            if ((*byte & 0x80u) == 0)
                return value;
        }
        return std::nullopt;
    }

  private:
    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
};

bool chunk_type_is(std::span<const std::uint8_t> type, const char (&expected)[5]) noexcept {
    for (std::size_t index = 0; index < kChunkTypeBytes; ++index) {
        if (type[index] != static_cast<std::uint8_t>(expected[index]))
            return false;
    }
    return true;
}

std::string chunk_type_text(std::span<const std::uint8_t> type) {
    std::string text;
    for (const auto byte : type) {
        if (byte >= 0x20u && byte < 0x7fu)
            text.push_back(static_cast<char>(byte));
        else
            text += byte_value(byte);
    }
    return text;
}

// Data bytes of a channel message, excluding the status byte.
std::size_t channel_message_data_bytes(std::uint8_t status) noexcept {
    switch (status & 0xf0u) {
    case 0xc0u:
    case 0xd0u:
        return 1;
    default:
        return 2;
    }
}

struct TrackDecoder {
    ByteReader reader;
    const SmfImportOptions& options;
    std::size_t& total_events;
    SmfDecodedTrack track{};
    std::int64_t tick = 0;
    std::uint32_t order = 0;
    std::uint8_t running_status = 0;
    bool saw_end_of_track = false;
    bool saw_track_name = false;

    // Counts every event the decoder walks, not only the retained ones, so an
    // out-of-subset flood under IgnoreNonNote is bounded the same way.
    std::optional<SmfError> count_event() {
        if (total_events >= options.limits.max_events)
            return smf_error(SmfErrorCode::LimitExceeded,
                             "event count exceeds max_events (" +
                                 decimal(static_cast<std::int64_t>(options.limits.max_events)) +
                                 ")");
        ++total_events;
        return std::nullopt;
    }

    void push(SmfDecodedEvent event) {
        event.tick = tick;
        event.order = order++;
        track.events.push_back(event);
    }

    std::optional<SmfError> ignore_or_reject(std::string description) {
        if (options.unsupported_events == SmfUnsupportedEventPolicy::IgnoreNonNote)
            return std::nullopt;
        return smf_error(SmfErrorCode::UnsupportedFeature,
                         description + " is outside the documented subset");
    }

    std::optional<SmfError> read_meta();
    std::optional<SmfError> read_system_exclusive(std::uint8_t status);
    std::optional<SmfError> read_channel_message(std::uint8_t status);
    std::optional<SmfError> run();
};

std::optional<SmfError> TrackDecoder::read_meta() {
    // Meta and system-exclusive events cancel running status: a following data
    // byte with no status byte is malformed, never a repeat of the last note.
    running_status = 0;
    const auto type = reader.read_u8();
    if (!type)
        return smf_error(SmfErrorCode::Truncated, "meta event type is missing");
    const auto length = reader.read_variable_length();
    if (!length)
        return smf_error(SmfErrorCode::MalformedEvent,
                         "meta event " + byte_value(*type) + " has a malformed length");
    if (*length > options.limits.max_payload_bytes)
        return smf_error(SmfErrorCode::LimitExceeded,
                         "meta event " + byte_value(*type) + " payload of " +
                             decimal(static_cast<std::int64_t>(*length)) +
                             " bytes exceeds max_payload_bytes");
    const auto payload = reader.read_bytes(*length);
    if (!payload)
        return smf_error(SmfErrorCode::Truncated,
                         "meta event " + byte_value(*type) + " payload is truncated");

    switch (*type) {
    case kMetaEndOfTrack:
        if (*length != 0)
            return smf_error(SmfErrorCode::MalformedEvent,
                             "end-of-track event carries a payload");
        saw_end_of_track = true;
        return std::nullopt;
    case kMetaSetTempo: {
        if (*length != 3)
            return smf_error(SmfErrorCode::MalformedEvent,
                             "set-tempo event length is " +
                                 decimal(static_cast<std::int64_t>(*length)) + ", expected 3");
        SmfDecodedEvent event{};
        event.message = SmfMessage::Tempo;
        event.microseconds_per_quarter = (static_cast<std::uint32_t>((*payload)[0]) << 16u) |
                                         (static_cast<std::uint32_t>((*payload)[1]) << 8u) |
                                         static_cast<std::uint32_t>((*payload)[2]);
        push(event);
        return std::nullopt;
    }
    case kMetaTimeSignature: {
        if (*length != 4)
            return smf_error(SmfErrorCode::MalformedEvent,
                             "time-signature event length is " +
                                 decimal(static_cast<std::int64_t>(*length)) + ", expected 4");
        SmfDecodedEvent event{};
        event.message = SmfMessage::TimeSignature;
        event.meter_numerator = (*payload)[0];
        event.meter_denominator_power = (*payload)[1];
        push(event);
        return std::nullopt;
    }
    case kMetaTrackName:
        if (*length > options.limits.max_track_name_bytes)
            return smf_error(SmfErrorCode::LimitExceeded,
                             "track name of " + decimal(static_cast<std::int64_t>(*length)) +
                                 " bytes exceeds max_track_name_bytes");
        if (saw_track_name)
            return smf_error(SmfErrorCode::MalformedEvent,
                             "track carries more than one name event");
        saw_track_name = true;
        track.name.assign(reinterpret_cast<const char*>(payload->data()), payload->size());
        return std::nullopt;
    default:
        return ignore_or_reject("meta event " + byte_value(*type));
    }
}

std::optional<SmfError> TrackDecoder::read_system_exclusive(std::uint8_t status) {
    running_status = 0;
    const auto length = reader.read_variable_length();
    if (!length)
        return smf_error(SmfErrorCode::MalformedEvent,
                         "system-exclusive event " + byte_value(status) +
                             " has a malformed length");
    if (*length > options.limits.max_payload_bytes)
        return smf_error(SmfErrorCode::LimitExceeded,
                         "system-exclusive payload of " +
                             decimal(static_cast<std::int64_t>(*length)) +
                             " bytes exceeds max_payload_bytes");
    if (!reader.read_bytes(*length))
        return smf_error(SmfErrorCode::Truncated, "system-exclusive payload is truncated");
    return ignore_or_reject("system-exclusive event " + byte_value(status));
}

std::optional<SmfError> TrackDecoder::read_channel_message(std::uint8_t status) {
    const auto data_bytes = channel_message_data_bytes(status);
    std::uint8_t data[2] = {0, 0};
    for (std::size_t index = 0; index < data_bytes; ++index) {
        const auto byte = reader.read_u8();
        if (!byte)
            return smf_error(SmfErrorCode::Truncated,
                             "channel message " + byte_value(status) + " is truncated");
        if (*byte >= 0x80u)
            return smf_error(SmfErrorCode::MalformedEvent,
                             "channel message " + byte_value(status) +
                                 " has a status byte where a data byte was expected");
        data[index] = *byte;
    }

    const auto kind = static_cast<std::uint8_t>(status & 0xf0u);
    if (kind != 0x80u && kind != 0x90u)
        return ignore_or_reject("channel message " + byte_value(kind));

    SmfDecodedEvent event{};
    event.channel = static_cast<std::uint8_t>(status & 0x0fu);
    event.pitch = data[0];
    event.velocity = data[1];
    // A Note On with velocity zero is a Note Off; the specification defines the
    // aliasing, so honouring it here is not a reinterpretation of intent.
    event.message = (kind == 0x90u && data[1] != 0) ? SmfMessage::NoteOn : SmfMessage::NoteOff;
    push(event);
    return std::nullopt;
}

std::optional<SmfError> TrackDecoder::run() {
    while (!reader.exhausted()) {
        if (saw_end_of_track)
            return smf_error(SmfErrorCode::MalformedEvent,
                             "track carries events after its end-of-track event");
        if (auto failure = count_event())
            return failure;
        const auto delta = reader.read_variable_length();
        if (!delta)
            return smf_error(SmfErrorCode::MalformedEvent, "event delta time is malformed");
        if (static_cast<std::int64_t>(*delta) > options.limits.max_smf_ticks - tick)
            return smf_error(SmfErrorCode::LimitExceeded,
                             "event position exceeds max_smf_ticks (" +
                                 decimal(options.limits.max_smf_ticks) + ")");
        tick += static_cast<std::int64_t>(*delta);

        const auto next = reader.peek_u8();
        if (!next)
            return smf_error(SmfErrorCode::Truncated, "event status byte is missing");
        std::uint8_t status = *next;
        if (status >= 0x80u) {
            reader.read_u8();
        } else {
            if (running_status == 0)
                return smf_error(SmfErrorCode::MalformedEvent,
                                 "data byte " + byte_value(status) +
                                     " with no running status in effect");
            status = running_status;
        }

        std::optional<SmfError> failure;
        if (status == kMetaStatus) {
            failure = read_meta();
        } else if (status == kSysExStatus || status == kSysExEscapeStatus) {
            failure = read_system_exclusive(status);
        } else if (status >= 0xf0u) {
            // System-common and realtime status bytes have no meaning in a file.
            failure = smf_error(SmfErrorCode::MalformedEvent,
                                "system status byte " + byte_value(status) +
                                    " is not valid inside a track chunk");
        } else {
            running_status = status;
            failure = read_channel_message(status);
        }
        if (failure)
            return failure;
    }
    if (!saw_end_of_track)
        return smf_error(SmfErrorCode::MalformedEvent, "track has no end-of-track event");
    return std::nullopt;
}

} // namespace

DecodeResult decode_smf(std::span<const std::uint8_t> file_bytes,
                        const SmfImportOptions& options) {
    if (file_bytes.size() > options.limits.max_file_bytes)
        return Err(smf_error(SmfErrorCode::LimitExceeded,
                             "file of " +
                                 decimal(static_cast<std::int64_t>(file_bytes.size())) +
                                 " bytes exceeds max_file_bytes"));

    ByteReader reader(file_bytes);
    const auto header_type = reader.read_bytes(kChunkTypeBytes);
    if (!header_type)
        return Err(smf_error(SmfErrorCode::MissingHeader, "data is too short for an MThd chunk"));
    if (!chunk_type_is(*header_type, "MThd"))
        return Err(smf_error(SmfErrorCode::MissingHeader,
                             "first chunk is '" + chunk_type_text(*header_type) +
                                 "', expected 'MThd'"));
    const auto header_length = reader.read_u32();
    if (!header_length)
        return Err(smf_error(SmfErrorCode::Truncated, "MThd length is truncated"));
    if (*header_length < kMinimumHeaderPayload)
        return Err(smf_error(SmfErrorCode::InvalidHeader,
                             "MThd length is " +
                                 decimal(static_cast<std::int64_t>(*header_length)) +
                                 ", expected at least 6"));
    const auto header_payload = reader.read_bytes(*header_length);
    if (!header_payload)
        return Err(smf_error(SmfErrorCode::Truncated, "MThd payload is truncated"));

    SmfDecodedFile file{};
    file.format = static_cast<std::uint16_t>((static_cast<std::uint16_t>((*header_payload)[0])
                                              << 8u) |
                                             (*header_payload)[1]);
    const auto declared_tracks = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>((*header_payload)[2]) << 8u) | (*header_payload)[3]);
    const auto raw_division = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>((*header_payload)[4]) << 8u) | (*header_payload)[5]);

    if (file.format > 1)
        return Err(smf_error(SmfErrorCode::UnsupportedFormat,
                             "MThd format " + decimal(file.format) +
                                 " is not a single or simultaneous-track file"));
    if (file.format == 0 && declared_tracks != 1)
        return Err(smf_error(SmfErrorCode::InvalidHeader,
                             "format 0 declares " + decimal(declared_tracks) +
                                 " tracks, expected 1"));
    if ((raw_division & 0x8000u) != 0)
        return Err(smf_error(SmfErrorCode::UnsupportedDivision,
                             "SMPTE division " + decimal(raw_division) +
                                 " has no musical tick grid"));
    if (raw_division == 0)
        return Err(smf_error(SmfErrorCode::UnsupportedDivision, "MThd division is zero"));
    file.division = raw_division;

    if (declared_tracks > options.limits.max_tracks)
        return Err(smf_error(SmfErrorCode::LimitExceeded,
                             "file declares " + decimal(declared_tracks) +
                                 " tracks, exceeding max_tracks (" +
                                 decimal(static_cast<std::int64_t>(options.limits.max_tracks)) +
                                 ")"));

    std::size_t total_events = 0;
    file.tracks.reserve(declared_tracks);
    for (std::uint16_t index = 0; index < declared_tracks; ++index) {
        const auto chunk_type = reader.read_bytes(kChunkTypeBytes);
        if (!chunk_type)
            return Err(smf_error(SmfErrorCode::Truncated,
                                 "file declares " + decimal(declared_tracks) +
                                     " tracks but ends after " + decimal(index)));
        if (!chunk_type_is(*chunk_type, "MTrk"))
            return Err(smf_error(SmfErrorCode::UnsupportedFeature,
                                 "chunk '" + chunk_type_text(*chunk_type) +
                                     "' is not an MTrk track chunk"));
        const auto chunk_length = reader.read_u32();
        if (!chunk_length)
            return Err(smf_error(SmfErrorCode::Truncated, "MTrk length is truncated"));
        const auto chunk = reader.read_bytes(*chunk_length);
        if (!chunk)
            return Err(smf_error(SmfErrorCode::Truncated,
                                 "MTrk chunk " + decimal(index) + " declares " +
                                     decimal(static_cast<std::int64_t>(*chunk_length)) +
                                     " bytes but the file ends first"));

        TrackDecoder decoder{ByteReader(*chunk), options, total_events};
        if (auto failure = decoder.run()) {
            failure->message = "track " + decimal(index) + ": " + failure->message;
            return Err(*failure);
        }
        file.tracks.push_back(std::move(decoder.track));
    }

    if (!reader.exhausted())
        return Err(smf_error(SmfErrorCode::MalformedEvent,
                             decimal(static_cast<std::int64_t>(reader.remaining())) +
                                 " trailing bytes follow the last declared track"));
    return Ok(std::move(file));
}

} // namespace pulp::timeline::detail
