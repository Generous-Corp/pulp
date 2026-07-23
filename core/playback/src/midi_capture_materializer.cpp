#include <pulp/playback/midi_capture_materializer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace pulp::playback {
namespace {

struct ActiveNote {
    std::uint64_t frame = 0;
    std::uint8_t velocity = 0;
};

struct RawNote {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint8_t velocity = 0;
    std::uint8_t pitch = 0;
    std::uint8_t channel = 0;
};

timebase::SamplePosition add_frames(timebase::SamplePosition start, std::uint64_t frames) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (frames > static_cast<std::uint64_t>(maximum) ||
        start.value > maximum - static_cast<std::int64_t>(frames))
        return {maximum};
    return {start.value + static_cast<std::int64_t>(frames)};
}

timebase::TickPosition quantize(timebase::TickPosition value,
                                timebase::TickDuration grid) noexcept {
    if (grid.value <= 0)
        return value;
    const auto scaled =
        static_cast<long double>(value.value) / static_cast<long double>(grid.value);
    const auto rounded = std::round(scaled);
    const auto result = rounded * static_cast<long double>(grid.value);
    if (result >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return {std::numeric_limits<std::int64_t>::max()};
    if (result <= static_cast<long double>(std::numeric_limits<std::int64_t>::min()))
        return {std::numeric_limits<std::int64_t>::min()};
    return {static_cast<std::int64_t>(result)};
}

bool is_mpe_expression(const midi::MidiEvent& event) noexcept {
    const auto status = event.data()[0] & 0xf0;
    return event.is_pitch_bend() || (event.is_cc() && event.cc_number() == 74) || status == 0xd0;
}

std::uint16_t expand_velocity(std::uint8_t velocity) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(velocity) * 65'535u + 63u) /
                                      127u);
}

} // namespace

runtime::Result<MaterializedMidiCapture, MidiCaptureMaterializationError>
materialize_midi_capture(std::span<const CapturedMidiEvent> events,
                         MidiCaptureMaterializationConfig config) {
    using Result = runtime::Result<MaterializedMidiCapture, MidiCaptureMaterializationError>;
    if (config.tempo_map == nullptr || config.frame_count == 0 ||
        config.frame_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        config.minimum_note_duration.value <= 0 || config.next_item_id == 0)
        return Result(runtime::Err(MidiCaptureMaterializationError::InvalidConfig));

    std::vector<CapturedMidiEvent> ordered(events.begin(), events.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const CapturedMidiEvent& lhs, const CapturedMidiEvent& rhs) {
                         return lhs.take_frame < rhs.take_frame;
                     });
    std::array<std::vector<ActiveNote>, 16 * 128> active;
    std::array<std::uint32_t, 16> active_on_channel{};
    std::vector<RawNote> raw_notes;
    std::vector<CapturedMidiEvent> expression;
    for (const auto& captured : ordered) {
        if (captured.take_frame >= config.frame_count)
            continue;
        const auto channel = captured.event.channel();
        if (captured.event.is_note_on() && captured.event.velocity() != 0) {
            const auto key = static_cast<std::size_t>(channel) * 128 + captured.event.note();
            active[key].push_back({captured.take_frame, captured.event.velocity()});
            ++active_on_channel[channel];
        } else if (captured.event.is_note_off() ||
                   (captured.event.is_note_on() && captured.event.velocity() == 0)) {
            const auto key = static_cast<std::size_t>(channel) * 128 + captured.event.note();
            if (!active[key].empty()) {
                const auto note = active[key].front();
                active[key].erase(active[key].begin());
                --active_on_channel[channel];
                raw_notes.push_back({note.frame, captured.take_frame, note.velocity,
                                     captured.event.note(), channel});
            }
        } else if (is_mpe_expression(captured.event) && active_on_channel[channel] != 0) {
            expression.push_back(captured);
        }
    }
    for (std::size_t key = 0; key < active.size(); ++key) {
        const auto channel = static_cast<std::uint8_t>(key / 128);
        const auto pitch = static_cast<std::uint8_t>(key % 128);
        for (const auto& note : active[key])
            raw_notes.push_back({note.frame, config.frame_count, note.velocity, pitch, channel});
    }
    std::sort(raw_notes.begin(), raw_notes.end(), [](const RawNote& lhs, const RawNote& rhs) {
        if (lhs.start != rhs.start)
            return lhs.start < rhs.start;
        if (lhs.channel != rhs.channel)
            return lhs.channel < rhs.channel;
        return lhs.pitch < rhs.pitch;
    });

    const auto clip_start = config.tempo_map->samples_to_ticks(config.placement_start);
    timeline::ItemIdAllocator ids(config.next_item_id);
    std::vector<timeline::NoteEvent> notes;
    notes.reserve(raw_notes.size());
    for (const auto& raw : raw_notes) {
        auto id = ids.allocate();
        if (!id)
            return Result(runtime::Err(MidiCaptureMaterializationError::IdentityExhausted));
        const auto start_offset =
            config.tempo_map->samples_to_ticks(add_frames(config.placement_start, raw.start)) -
            clip_start;
        const auto end_offset =
            config.tempo_map->samples_to_ticks(add_frames(config.placement_start, raw.end)) -
            clip_start;
        timebase::TickPosition start{start_offset.value};
        timebase::TickPosition end{end_offset.value};
        start = quantize(start, config.quantize_grid);
        end = quantize(end, config.quantize_grid);
        const auto minimum_duration = config.minimum_note_duration.value;
        const auto minimum_end =
            start.value > std::numeric_limits<std::int64_t>::max() - minimum_duration
                ? std::numeric_limits<std::int64_t>::max()
                : start.value + minimum_duration;
        if (end.value < minimum_end)
            end = {minimum_end};
        notes.push_back({
            std::move(id).value(),
            start,
            end - start,
            expand_velocity(raw.velocity),
            raw.pitch,
            raw.channel,
        });
    }
    auto content = timeline::NoteContent::create(std::move(notes));
    if (!content)
        return Result(runtime::Err(MidiCaptureMaterializationError::InvalidNotes));
    return Result(runtime::Ok(MaterializedMidiCapture{
        clip_start,
        std::move(content).value(),
        std::move(expression),
        ids.next_value(),
    }));
}

} // namespace pulp::playback
