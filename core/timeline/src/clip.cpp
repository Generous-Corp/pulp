#include <pulp/timeline/clip.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <variant>

namespace pulp::timeline {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Err(ModelError{code, item, related});
}

bool positive_range(std::int64_t start, std::int64_t duration) noexcept {
    return duration > 0 && start <= std::numeric_limits<std::int64_t>::max() - duration;
}

bool valid_playback_properties(ClipPlaybackProperties playback, std::uint64_t duration) noexcept {
    if (!std::isfinite(playback.gain_linear) || playback.gain_linear < 0.0f)
        return false;
    return playback.fade_in_duration <= duration && playback.fade_out_duration <= duration;
}

bool valid_time_conform(TimeConform time_conform, ClipTimeAnchor anchor,
                        const ClipContent& content) noexcept {
    switch (time_conform) {
        case TimeConform::None:
            return true;
        case TimeConform::Resample:
        case TimeConform::Stretch:
            return anchor == ClipTimeAnchor::Musical &&
                   std::holds_alternative<MediaRef>(content);
    }
    return false;
}

} // namespace

struct Clip::Data {
    ItemId id;
    ClipTimeRange range;
    ClipContent content;
    ClipPlaybackProperties playback;
    TimeConform time_conform = TimeConform::None;
};

runtime::Result<Clip, ModelError> Clip::create(ItemId id, timebase::TickPosition start,
                                               timebase::TickDuration duration, ClipContent content,
                                               ClipPlaybackProperties playback,
                                               TimeConform time_conform) {
    if (!id.valid())
        return fail<Clip>(ModelErrorCode::InvalidItemId, id);
    if (!positive_range(start.value, duration.value))
        return fail<Clip>(ModelErrorCode::InvalidDuration, id);
    if (!valid_playback_properties(playback, static_cast<std::uint64_t>(duration.value)))
        return fail<Clip>(ModelErrorCode::InvalidClipPlaybackProperties, id);
    if (!valid_time_conform(time_conform, ClipTimeAnchor::Musical, content))
        return fail<Clip>(ModelErrorCode::InvalidTimeConform, id);
    if (const auto* media = std::get_if<MediaRef>(&content)) {
        if (!media->asset_id.valid() || media->source_start.value < 0 || media->frame_count == 0 ||
            static_cast<std::uint64_t>(media->source_start.value) >
                std::numeric_limits<std::uint64_t>::max() - media->frame_count)
            return fail<Clip>(ModelErrorCode::InvalidMediaRange, id, media->asset_id);
    }
    if (const auto* reference = std::get_if<SequenceRef>(&content)) {
        if (!reference->sequence_id.valid() || reference->source_start.value < 0)
            return fail<Clip>(ModelErrorCode::MissingSequenceReference, id, reference->sequence_id);
        if (duration.value >
            std::numeric_limits<std::int64_t>::max() - reference->source_start.value)
            return fail<Clip>(ModelErrorCode::InvalidDuration, id, reference->sequence_id);
    }
    return runtime::Result<Clip, ModelError>(runtime::Ok(Clip(std::make_shared<const Data>(
        Data{id, MusicalTimeRange{start, duration}, std::move(content), playback, time_conform}))));
}

runtime::Result<Clip, ModelError> Clip::create_absolute(ItemId id, timebase::SamplePosition start,
                                                        std::uint64_t sample_count,
                                                        timebase::RationalRate sample_rate,
                                                        ClipContent content,
                                                        ClipPlaybackProperties playback,
                                                        TimeConform time_conform) {
    if (!id.valid())
        return fail<Clip>(ModelErrorCode::InvalidItemId, id);
    if (!sample_rate.valid())
        return fail<Clip>(ModelErrorCode::InvalidSampleRate, id);
    if (sample_count == 0 ||
        sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        start.value >
            std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(sample_count))
        return fail<Clip>(ModelErrorCode::InvalidDuration, id);
    if (!valid_playback_properties(playback, sample_count))
        return fail<Clip>(ModelErrorCode::InvalidClipPlaybackProperties, id);
    if (!valid_time_conform(time_conform, ClipTimeAnchor::Absolute, content))
        return fail<Clip>(ModelErrorCode::InvalidTimeConform, id);
    if (const auto* media = std::get_if<MediaRef>(&content)) {
        if (!media->asset_id.valid() || media->source_start.value < 0 || media->frame_count == 0 ||
            static_cast<std::uint64_t>(media->source_start.value) >
                std::numeric_limits<std::uint64_t>::max() - media->frame_count)
            return fail<Clip>(ModelErrorCode::InvalidMediaRange, id, media->asset_id);
    }
    if (const auto* reference = std::get_if<SequenceRef>(&content))
        return fail<Clip>(ModelErrorCode::InvalidDuration, id, reference->sequence_id);
    return runtime::Result<Clip, ModelError>(runtime::Ok(Clip(std::make_shared<const Data>(
        Data{id, AbsoluteTimeRange{start, sample_count, sample_rate.normalized()},
             std::move(content), playback, time_conform}))));
}

ItemId Clip::id() const noexcept {
    return data_->id;
}

ClipTimeAnchor Clip::time_anchor() const noexcept {
    return std::holds_alternative<MusicalTimeRange>(data_->range) ? ClipTimeAnchor::Musical
                                                                  : ClipTimeAnchor::Absolute;
}

const ClipTimeRange& Clip::time_range() const noexcept {
    return data_->range;
}

timebase::TickPosition Clip::start() const noexcept {
    const auto* range = std::get_if<MusicalTimeRange>(&data_->range);
    return range ? range->start : timebase::TickPosition{};
}

timebase::TickDuration Clip::duration() const noexcept {
    const auto* range = std::get_if<MusicalTimeRange>(&data_->range);
    return range ? range->duration : timebase::TickDuration{};
}

timebase::TickPosition Clip::end() const noexcept {
    return start() + duration();
}

timebase::SamplePosition Clip::absolute_start() const noexcept {
    const auto* range = std::get_if<AbsoluteTimeRange>(&data_->range);
    return range ? range->start : timebase::SamplePosition{};
}

std::uint64_t Clip::absolute_duration_samples() const noexcept {
    const auto* range = std::get_if<AbsoluteTimeRange>(&data_->range);
    return range ? range->sample_count : 0;
}

timebase::RationalRate Clip::absolute_sample_rate() const noexcept {
    const auto* range = std::get_if<AbsoluteTimeRange>(&data_->range);
    return range ? range->sample_rate : timebase::RationalRate{0, 1};
}

timebase::SamplePosition Clip::absolute_end() const noexcept {
    return {absolute_start().value + static_cast<std::int64_t>(absolute_duration_samples())};
}

const ClipContent& Clip::content() const noexcept {
    return data_->content;
}

ClipPlaybackProperties Clip::playback_properties() const noexcept {
    return data_->playback;
}

TimeConform Clip::time_conform() const noexcept {
    return data_->time_conform;
}

runtime::Result<Clip, ModelError> Clip::with_time_range(ClipTimeRange range) const {
    if (const auto* musical = std::get_if<MusicalTimeRange>(&range))
        return create(id(), musical->start, musical->duration, content(), playback_properties(),
                      time_conform());
    const auto& absolute = std::get<AbsoluteTimeRange>(range);
    return create_absolute(id(), absolute.start, absolute.sample_count, absolute.sample_rate,
                           content(), playback_properties(), time_conform());
}

runtime::Result<Clip, ModelError> Clip::with_content(ClipContent replacement) const {
    if (time_anchor() == ClipTimeAnchor::Musical)
        return create(id(), start(), duration(), std::move(replacement), playback_properties(),
                      time_conform());
    return create_absolute(id(), absolute_start(), absolute_duration_samples(),
                           absolute_sample_rate(), std::move(replacement), playback_properties(),
                           time_conform());
}

runtime::Result<Clip, ModelError>
Clip::with_playback_properties(ClipPlaybackProperties playback) const {
    if (time_anchor() == ClipTimeAnchor::Musical)
        return create(id(), start(), duration(), content(), playback, time_conform());
    return create_absolute(id(), absolute_start(), absolute_duration_samples(),
                           absolute_sample_rate(), content(), playback, time_conform());
}

runtime::Result<Clip, ModelError> Clip::with_time_conform(TimeConform time_conform) const {
    if (time_anchor() == ClipTimeAnchor::Musical)
        return create(id(), start(), duration(), content(), playback_properties(), time_conform);
    return create_absolute(id(), absolute_start(), absolute_duration_samples(),
                           absolute_sample_rate(), content(), playback_properties(), time_conform);
}

} // namespace pulp::timeline
