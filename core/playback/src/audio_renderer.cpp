#include <pulp/playback/audio_renderer.hpp>

#include "audio_renderer_internal.hpp"

#include <pulp/audio/sample_rate_conversion.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>

namespace pulp::playback {

struct detail::AudioSampleRateConverterCache::Impl {
    struct Entry {
        timebase::RationalRate source;
        timebase::RationalRate target;
        std::shared_ptr<const audio::PreparedSampleRateConversion> converter;
    };

    struct HostRateEntry {
        std::shared_ptr<const audio::AudioFileData> source;
        std::uint64_t source_start = 0;
        std::uint64_t source_frames = 0;
        std::shared_ptr<const audio::PreparedVariableRateConversion> converter;
    };

    std::size_t converter_count() const noexcept {
        return entries.size() + host_rate_entries.size() + (fixed_rate_builder ? 1u : 0u) +
               (host_rate_builder ? 1u : 0u);
    }

    auto find_host(const std::shared_ptr<const audio::AudioFileData>& source,
                   std::uint64_t source_start, std::uint64_t source_frames) {
        return std::find_if(
            host_rate_entries.begin(), host_rate_entries.end(), [&](const HostRateEntry& entry) {
                return entry.source == source && entry.source_start == source_start &&
                       entry.source_frames == source_frames;
            });
    }

    static std::uint64_t saturating_add(std::uint64_t lhs, std::uint64_t rhs) noexcept {
        return rhs <= std::numeric_limits<std::uint64_t>::max() - lhs
                   ? lhs + rhs
                   : std::numeric_limits<std::uint64_t>::max();
    }

    template <typename T>
    static std::uint64_t vector_storage_bytes(const std::vector<T>& values) noexcept {
        if (values.capacity() >
            (std::numeric_limits<std::uint64_t>::max() - audio::kConverterAllocationOverhead) /
                sizeof(T))
            return std::numeric_limits<std::uint64_t>::max();
        return values.capacity() * sizeof(T) +
               (values.capacity() != 0 ? audio::kConverterAllocationOverhead : 0u);
    }

    std::uint64_t cache_storage_bytes() const noexcept {
        return saturating_add(vector_storage_bytes(entries),
                              vector_storage_bytes(host_rate_entries));
    }

    std::uint64_t required_bytes(std::uint64_t additional = 0) const noexcept {
        return saturating_add(saturating_add(converter_bytes, cache_storage_bytes()), additional);
    }

    runtime::Result<std::shared_ptr<const audio::PreparedSampleRateConversion>, AudioRendererError>
    get(timebase::RationalRate source, timebase::RationalRate target, timeline::ItemId item,
        timeline::ItemId related_item, const AudioRendererLimits& limits) {
        while (true) {
            auto prepared = prepare(source, target, item, related_item, limits);
            if (!prepared)
                return runtime::Err(prepared.error());
            if (*prepared)
                break;
        }
        source = source.normalized();
        target = target.normalized();
        if (source == target)
            return runtime::Ok(std::shared_ptr<const audio::PreparedSampleRateConversion>{});
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.source == source && entry.target == target;
        });
        return runtime::Ok(found->converter);
    }

    runtime::Result<bool, AudioRendererError>
    prepare(timebase::RationalRate source, timebase::RationalRate target, timeline::ItemId item,
            timeline::ItemId related_item, const AudioRendererLimits& limits) {
        source = source.normalized();
        target = target.normalized();
        if (source == target)
            return runtime::Ok(true);
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.source == source && entry.target == target;
        });
        if (found != entries.end())
            return runtime::Ok(true);

        // Scale the reconstruction cutoff to the target Nyquist when
        // decimating. The 64-tap Kaiser kernel supplies the transition band
        // before frequencies far above that boundary can fold into output.
        const auto ratio = static_cast<double>(target.as_long_double() / source.as_long_double());
        const auto cutoff = std::min(1.0, ratio);
        if (cutoff < audio::PreparedSampleRateConversion::kMinimumSupportedCutoff)
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::UnsupportedSampleRate,
                item,
                related_item,
            });
        const auto same_active =
            fixed_rate_builder && fixed_rate_source == source && fixed_rate_target == target;
        if (!same_active) {
            if (fixed_rate_builder)
                return runtime::Ok(false);
            if (converter_count() >= limits.max_sample_rate_converters) {
                return runtime::Err(AudioRendererError{
                    AudioRendererErrorCode::CapacityExceeded,
                    item,
                    related_item,
                    converter_count() + 1u,
                    limits.max_sample_rate_converters,
                });
            }
            const auto estimated_bytes =
                audio::PreparedSampleRateConversion::estimated_prepared_bytes();
            entries.reserve(entries.size() + 1u);
            const auto required = required_bytes(estimated_bytes);
            if (required > limits.max_sample_rate_converter_bytes) {
                return runtime::Err(AudioRendererError{
                    AudioRendererErrorCode::CapacityExceeded,
                    item,
                    related_item,
                    required,
                    limits.max_sample_rate_converter_bytes,
                });
            }
            fixed_rate_source = source;
            fixed_rate_target = target;
            fixed_rate_builder = std::make_unique<audio::SampleRateConversionBuilder>(cutoff);
        }
        if (!fixed_rate_builder->step())
            return runtime::Ok(false);
        auto converter = fixed_rate_builder->take();
        if (!converter)
            return runtime::Err(AudioRendererError{AudioRendererErrorCode::UnsupportedSampleRate,
                                                   item, related_item});
        const auto actual_bytes = converter->prepared_bytes();
        const auto required = required_bytes(actual_bytes);
        if (required > limits.max_sample_rate_converter_bytes)
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::CapacityExceeded,
                item,
                related_item,
                required,
                limits.max_sample_rate_converter_bytes,
            });
        entries.push_back({source, target, std::move(converter)});
        converter_bytes += actual_bytes;
        fixed_rate_builder.reset();
        fixed_rate_source = {};
        fixed_rate_target = {};
        return runtime::Ok(true);
    }

    runtime::Result<std::shared_ptr<const audio::PreparedSampleRateConversion>, AudioRendererError>
    seed(timebase::RationalRate source, timebase::RationalRate target,
         std::shared_ptr<const audio::PreparedSampleRateConversion> converter,
         timeline::ItemId item, timeline::ItemId related_item, const AudioRendererLimits& limits) {
        source = source.normalized();
        target = target.normalized();
        if (source == target)
            return runtime::Ok(std::shared_ptr<const audio::PreparedSampleRateConversion>{});
        if (!converter)
            return runtime::Err(
                AudioRendererError{AudioRendererErrorCode::InvalidAsset, item, related_item});
        if (!converter->ready())
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::UnsupportedSampleRate,
                item,
                related_item,
            });
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.source == source && entry.target == target;
        });
        if (found != entries.end())
            return runtime::Ok(found->converter);
        if (converter_count() >= limits.max_sample_rate_converters) {
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::CapacityExceeded,
                item,
                related_item,
                converter_count() + 1u,
                limits.max_sample_rate_converters,
            });
        }
        entries.reserve(entries.size() + 1u);
        const auto required = required_bytes(converter->prepared_bytes());
        if (required > limits.max_sample_rate_converter_bytes) {
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::CapacityExceeded,
                item,
                related_item,
                required,
                limits.max_sample_rate_converter_bytes,
            });
        }
        converter_bytes += converter->prepared_bytes();
        entries.push_back({source, target, converter});
        return runtime::Ok(std::move(converter));
    }

    runtime::Result<bool, AudioRendererError>
    prepare_host(std::shared_ptr<const audio::AudioFileData> source, std::uint64_t source_start,
                 std::uint64_t source_frames, timeline::ItemId item, timeline::ItemId related_item,
                 const AudioRendererLimits& limits) {
        if (!source || source_frames == 0 || source_start > source->num_frames() ||
            source_frames > source->num_frames() - source_start)
            return runtime::Err(
                AudioRendererError{AudioRendererErrorCode::InvalidAsset, item, related_item});
        if (find_host(source, source_start, source_frames) != host_rate_entries.end())
            return runtime::Ok(true);

        const auto same_active = host_rate_builder && host_rate_source == source &&
                                 host_rate_source_start == source_start &&
                                 host_rate_source_frames == source_frames;
        if (!same_active) {
            if (host_rate_builder)
                return runtime::Ok(false);
            if (converter_count() >= limits.max_sample_rate_converters) {
                return runtime::Err(AudioRendererError{
                    AudioRendererErrorCode::CapacityExceeded,
                    item,
                    related_item,
                    converter_count() + 1u,
                    limits.max_sample_rate_converters,
                });
            }
            const auto bytes = audio::PreparedVariableRateConversion::prepared_bytes(
                source_frames, source->channels.size());
            host_rate_entries.reserve(host_rate_entries.size() + 1u);
            const auto required =
                bytes ? required_bytes(*bytes) : std::numeric_limits<std::uint64_t>::max();
            if (!bytes || required > limits.max_sample_rate_converter_bytes) {
                return runtime::Err(AudioRendererError{
                    AudioRendererErrorCode::CapacityExceeded,
                    item,
                    related_item,
                    required,
                    limits.max_sample_rate_converter_bytes,
                });
            }
            host_rate_source = source;
            host_rate_source_start = source_start;
            host_rate_source_frames = source_frames;
            host_rate_builder = std::make_unique<audio::VariableRateConversionBuilder>(
                source, source_start, source_frames);
        }

        if (!host_rate_builder->step())
            return runtime::Ok(false);
        auto converter = host_rate_builder->take();
        if (!converter)
            return runtime::Err(
                AudioRendererError{AudioRendererErrorCode::InvalidAsset, item, related_item});
        const auto actual_bytes = converter->prepared_bytes();
        const auto required = required_bytes(actual_bytes);
        if (required > limits.max_sample_rate_converter_bytes)
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::CapacityExceeded,
                item,
                related_item,
                required,
                limits.max_sample_rate_converter_bytes,
            });
        host_rate_entries.push_back(
            {host_rate_source, host_rate_source_start, host_rate_source_frames, converter});
        converter_bytes += actual_bytes;
        host_rate_builder.reset();
        host_rate_source.reset();
        host_rate_source_start = 0;
        host_rate_source_frames = 0;
        return runtime::Ok(true);
    }

    runtime::Result<std::shared_ptr<const audio::PreparedVariableRateConversion>,
                    AudioRendererError>
    get_host(std::shared_ptr<const audio::AudioFileData> source, std::uint64_t source_start,
             std::uint64_t source_frames, timeline::ItemId item, timeline::ItemId related_item,
             const AudioRendererLimits& limits) {
        while (true) {
            auto prepared =
                prepare_host(source, source_start, source_frames, item, related_item, limits);
            if (!prepared)
                return runtime::Err(prepared.error());
            if (*prepared)
                break;
        }
        const auto found = find_host(source, source_start, source_frames);
        return runtime::Ok(found->converter);
    }

    runtime::Result<std::shared_ptr<const audio::PreparedVariableRateConversion>,
                    AudioRendererError>
    seed_host(std::shared_ptr<const audio::AudioFileData> source, std::uint64_t source_start,
              std::uint64_t source_frames,
              std::shared_ptr<const audio::PreparedVariableRateConversion> converter,
              timeline::ItemId item, timeline::ItemId related_item,
              const AudioRendererLimits& limits) {
        if (!source || !converter || source_frames == 0 || source_start > source->num_frames() ||
            source_frames > source->num_frames() - source_start)
            return runtime::Err(
                AudioRendererError{AudioRendererErrorCode::InvalidAsset, item, related_item});
        const auto found = find_host(source, source_start, source_frames);
        if (found != host_rate_entries.end())
            return runtime::Ok(found->converter);
        if (converter_count() >= limits.max_sample_rate_converters) {
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::CapacityExceeded,
                item,
                related_item,
                converter_count() + 1u,
                limits.max_sample_rate_converters,
            });
        }
        host_rate_entries.reserve(host_rate_entries.size() + 1u);
        const auto required = required_bytes(converter->prepared_bytes());
        if (required > limits.max_sample_rate_converter_bytes) {
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::CapacityExceeded,
                item,
                related_item,
                required,
                limits.max_sample_rate_converter_bytes,
            });
        }
        converter_bytes += converter->prepared_bytes();
        host_rate_entries.push_back({std::move(source), source_start, source_frames, converter});
        return runtime::Ok(std::move(converter));
    }

    std::vector<Entry> entries;
    std::vector<HostRateEntry> host_rate_entries;
    std::unique_ptr<audio::SampleRateConversionBuilder> fixed_rate_builder;
    timebase::RationalRate fixed_rate_source;
    timebase::RationalRate fixed_rate_target;
    std::unique_ptr<audio::VariableRateConversionBuilder> host_rate_builder;
    std::shared_ptr<const audio::AudioFileData> host_rate_source;
    std::uint64_t host_rate_source_start = 0;
    std::uint64_t host_rate_source_frames = 0;
    std::uint64_t converter_bytes = 0;
};

detail::AudioSampleRateConverterCache::AudioSampleRateConverterCache()
    : impl_(std::make_unique<Impl>()) {}
detail::AudioSampleRateConverterCache::~AudioSampleRateConverterCache() = default;
detail::AudioSampleRateConverterCache::AudioSampleRateConverterCache(
    AudioSampleRateConverterCache&&) noexcept = default;
detail::AudioSampleRateConverterCache& detail::AudioSampleRateConverterCache::operator=(
    AudioSampleRateConverterCache&&) noexcept = default;
runtime::Result<bool, AudioRendererError> detail::AudioSampleRateConverterCache::prepare(
    timebase::RationalRate source, timebase::RationalRate target, timeline::ItemId item,
    timeline::ItemId related_item, const AudioRendererLimits& limits) {
    return impl_->prepare(source, target, item, related_item, limits);
}
runtime::Result<std::shared_ptr<const audio::PreparedSampleRateConversion>, AudioRendererError>
detail::AudioSampleRateConverterCache::seed(
    timebase::RationalRate source, timebase::RationalRate target,
    std::shared_ptr<const audio::PreparedSampleRateConversion> converter, timeline::ItemId item,
    timeline::ItemId related_item, const AudioRendererLimits& limits) {
    return impl_->seed(source, target, std::move(converter), item, related_item, limits);
}

runtime::Result<std::shared_ptr<const audio::PreparedVariableRateConversion>, AudioRendererError>
detail::AudioSampleRateConverterCache::seed_host(
    std::shared_ptr<const audio::AudioFileData> source, std::uint64_t source_start,
    std::uint64_t source_frames,
    std::shared_ptr<const audio::PreparedVariableRateConversion> converter, timeline::ItemId item,
    timeline::ItemId related_item, const AudioRendererLimits& limits) {
    return impl_->seed_host(std::move(source), source_start, source_frames, std::move(converter),
                            item, related_item, limits);
}

runtime::Result<bool, AudioRendererError> detail::AudioSampleRateConverterCache::prepare_host(
    std::shared_ptr<const audio::AudioFileData> source, std::uint64_t source_start,
    std::uint64_t source_frames, timeline::ItemId item, timeline::ItemId related_item,
    const AudioRendererLimits& limits) {
    return impl_->prepare_host(std::move(source), source_start, source_frames, item, related_item,
                               limits);
}

namespace {

template <typename T>
runtime::Result<T, AudioRendererError> fail(AudioRendererErrorCode code, timeline::ItemId item = {},
                                            timeline::ItemId related = {}, std::uint64_t actual = 0,
                                            std::uint64_t limit = 0) {
    return runtime::Err(AudioRendererError{code, item, related, actual, limit});
}

bool valid_audio(const audio::AudioFileData& value, const AudioRendererLimits& limits) noexcept {
    if (value.sample_rate == 0 || value.channels.empty() ||
        value.channels.size() > limits.max_channels || value.num_frames() == 0 ||
        value.num_frames() > limits.max_asset_frames)
        return false;
    const auto frames = value.channels.front().size();
    return std::all_of(value.channels.begin(), value.channels.end(),
                       [frames](const auto& channel) { return channel.size() == frames; });
}

struct ResolvedAudioMedia {
    const DecodedAudioAsset* decoded = nullptr;
    std::uint64_t source_start = 0;
    timebase::RationalRate source_rate;
};

runtime::Result<ResolvedAudioMedia, AudioRendererError>
resolve_audio_media(timeline::ItemId item, const timeline::MediaRef& media,
                    const timeline::Project& project, const DecodedAudioAssetPool& assets,
                    const AudioRendererLimits& limits,
                    AudioRendererErrorCode range_error = AudioRendererErrorCode::InvalidClipRange) {
    const auto* metadata = project.find_asset(media.asset_id);
    const auto* decoded = assets.find(media.asset_id);
    if (!metadata || !decoded)
        return fail<ResolvedAudioMedia>(AudioRendererErrorCode::MissingDecodedAsset, item,
                                        media.asset_id);
    if (!valid_audio(*decoded->audio, limits))
        return fail<ResolvedAudioMedia>(AudioRendererErrorCode::InvalidAsset, item, media.asset_id);
    if (metadata->frame_count != decoded->audio->num_frames() ||
        metadata->sample_rate.denominator != 1 ||
        metadata->sample_rate.numerator != decoded->audio->sample_rate)
        return fail<ResolvedAudioMedia>(AudioRendererErrorCode::AssetMetadataMismatch, item,
                                        media.asset_id);
    const auto source_start = static_cast<std::uint64_t>(media.source_start.value);
    if (source_start > decoded->audio->num_frames() ||
        media.frame_count > decoded->audio->num_frames() - source_start)
        return fail<ResolvedAudioMedia>(range_error, item, media.asset_id);
    return runtime::Ok(
        ResolvedAudioMedia{decoded, source_start, metadata->sample_rate.normalized()});
}

bool same_rate(timebase::RationalRate lhs, timebase::RationalRate rhs) noexcept {
    return lhs.normalized() == rhs.normalized();
}

std::uint64_t positive_distance(std::int64_t start, std::int64_t end) noexcept {
    if (end <= start)
        return 0;
    return static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(start);
}

bool add_fits(std::int64_t start, std::uint64_t count) noexcept {
    return count <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
           start <= std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(count);
}

float envelope(const AudioClipRendererProgram& clip, std::uint64_t relative) noexcept {
    float value = clip.gain_linear;
    if (clip.fade_in_frames != 0 && relative < clip.fade_in_frames)
        value *= static_cast<float>(relative) / static_cast<float>(clip.fade_in_frames);
    const auto remaining = clip.timeline_frame_count - 1u - relative;
    if (clip.fade_out_frames != 0 && remaining < clip.fade_out_frames)
        value *= static_cast<float>(remaining) / static_cast<float>(clip.fade_out_frames);
    return value;
}

float envelope(const AudioClipRendererProgram& clip, long double relative) noexcept {
    auto value = static_cast<long double>(clip.gain_linear);
    if (clip.fade_in_frames != 0 && relative < clip.fade_in_frames)
        value *= relative / static_cast<long double>(clip.fade_in_frames);
    const auto remaining =
        std::max(0.0L, static_cast<long double>(clip.timeline_frame_count - 1u) - relative);
    if (clip.fade_out_frames != 0 && remaining < clip.fade_out_frames)
        value *= remaining / static_cast<long double>(clip.fade_out_frames);
    return static_cast<float>(value);
}

float source_sample(
    const AudioClipRendererProgram& clip, std::size_t output_channel, std::size_t output_channels,
    std::uint64_t frame, std::uint64_t next_frame, float fraction, double source_position,
    std::optional<double> host_source_frames_per_output_frame = std::nullopt) noexcept {
    const auto& source = *clip.audio;
    const auto source_channels = source.channels.size();
    auto sample = [&](std::size_t channel) {
        const auto segment = std::span<const float>(source.channels[channel])
                                 .subspan(static_cast<std::size_t>(clip.source_start),
                                          static_cast<std::size_t>(clip.source_frame_count));
        if (clip.host_rate_converter && host_source_frames_per_output_frame &&
            (*host_source_frames_per_output_frame > 1.0 ||
             clip.source_frames_per_timeline_frame > 1.0))
            return clip.host_rate_converter->read(channel, source_position,
                                                  *host_source_frames_per_output_frame);
        if (clip.sample_rate_converter) {
            return clip.sample_rate_converter->read(segment, source_position);
        }
        const auto first = source.channels[channel][frame];
        return first + (source.channels[channel][next_frame] - first) * fraction;
    };
    if (source_channels == 1)
        return sample(0);
    if (output_channels == 1) {
        float sum = 0.0f;
        for (std::size_t channel = 0; channel < source_channels; ++channel)
            sum += sample(channel);
        return sum / static_cast<float>(source_channels);
    }
    return output_channel < source_channels ? sample(output_channel) : 0.0f;
}

std::optional<std::int64_t> scale_position(std::int64_t value, timebase::RationalRate from,
                                           timebase::RationalRate to) noexcept {
    const auto scaled =
        static_cast<long double>(value) * to.as_long_double() / from.as_long_double();
    if (!std::isfinite(scaled) ||
        scaled < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        scaled > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return static_cast<std::int64_t>(std::floor(scaled));
}

struct WideUnsigned {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
};

WideUnsigned multiply_wide(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    constexpr std::uint64_t mask = std::numeric_limits<std::uint32_t>::max();
    const auto lhs_low = lhs & mask;
    const auto lhs_high = lhs >> 32u;
    const auto rhs_low = rhs & mask;
    const auto rhs_high = rhs >> 32u;
    const auto low_product = lhs_low * rhs_low;
    const auto middle = lhs_high * rhs_low + (low_product >> 32u);
    const auto middle_low = (middle & mask) + lhs_low * rhs_high;
    return {lhs_high * rhs_high + (middle >> 32u) + (middle_low >> 32u),
            (middle_low << 32u) | (low_product & mask)};
}

bool wide_less(WideUnsigned lhs, WideUnsigned rhs) noexcept {
    return lhs.high < rhs.high || (lhs.high == rhs.high && lhs.low < rhs.low);
}

WideUnsigned subtract_wide(WideUnsigned lhs, WideUnsigned rhs) noexcept {
    return {lhs.high - rhs.high - (lhs.low < rhs.low), lhs.low - rhs.low};
}

struct WideDivision {
    WideUnsigned quotient;
    WideUnsigned remainder;
};

WideDivision divide_wide(WideUnsigned numerator, WideUnsigned denominator) noexcept {
    WideDivision result;
    if ((denominator.high & (std::uint64_t{1} << 63u)) != 0) {
        if (!wide_less(numerator, denominator)) {
            result.quotient.low = 1;
            result.remainder = subtract_wide(numerator, denominator);
        } else {
            result.remainder = numerator;
        }
        return result;
    }
    for (int bit = 127; bit >= 0; --bit) {
        const auto input_bit =
            bit >= 64 ? (numerator.high >> (bit - 64)) & 1u : (numerator.low >> bit) & 1u;
        result.remainder = {result.remainder.high << 1u | result.remainder.low >> 63u,
                            result.remainder.low << 1u | input_bit};
        if (!wide_less(result.remainder, denominator)) {
            result.remainder = subtract_wide(result.remainder, denominator);
            if (bit >= 64)
                result.quotient.high |= std::uint64_t{1} << (bit - 64);
            else
                result.quotient.low |= std::uint64_t{1} << bit;
        }
    }
    return result;
}

// Media metadata guarantees an integer source rate. Use a portable 128-bit
// quotient here so large legal timeline positions never pass through floating
// point before the checked floor operation.
std::optional<std::int64_t> scale_media_position(std::int64_t value, timebase::RationalRate from,
                                                 timebase::RationalRate to) noexcept {
    from = from.normalized();
    to = to.normalized();
    if (!from.valid() || !to.valid() || from.denominator != 1)
        return std::nullopt;
    const bool negative = value < 0;
    const auto magnitude = negative ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
                                    : static_cast<std::uint64_t>(value);
    const auto numerator = multiply_wide(magnitude, to.numerator);
    const auto denominator = multiply_wide(from.numerator, to.denominator);
    if (denominator.high == 0 && denominator.low == 0)
        return std::nullopt;
    auto divided = divide_wide(numerator, denominator);
    const bool has_remainder = divided.remainder.high != 0 || divided.remainder.low != 0;
    if (negative && has_remainder) {
        ++divided.quotient.low;
        if (divided.quotient.low == 0)
            ++divided.quotient.high;
    }
    const auto limit = negative
                           ? std::uint64_t{1} << 63u
                           : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (divided.quotient.high != 0 || divided.quotient.low > limit)
        return std::nullopt;
    if (!negative)
        return static_cast<std::int64_t>(divided.quotient.low);
    if (divided.quotient.low == (std::uint64_t{1} << 63u))
        return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(divided.quotient.low);
}

std::optional<std::uint64_t> scale_duration_ceil(std::uint64_t value, timebase::RationalRate from,
                                                 timebase::RationalRate to) noexcept {
    const auto scaled =
        static_cast<long double>(value) * to.as_long_double() / from.as_long_double();
    if (!std::isfinite(scaled) || scaled <= 0.0L ||
        scaled > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        return std::nullopt;
    return static_cast<std::uint64_t>(std::ceil(scaled));
}

bool valid_transport(const TransportSnapshot& transport, std::size_t output_frames,
                     const timebase::CompiledTempoMap& tempo_map,
                     const AudioRendererLimits& limits) noexcept {
    if (!valid_transport_ranges(transport) || transport.frame_count != output_frames ||
        transport.frame_count > limits.max_block_frames || transport.tempo_map != &tempo_map ||
        !same_rate(transport.sample_rate, tempo_map.sample_rate()))
        return false;
    std::uint32_t prior_end = 0;
    for (std::uint8_t index = 0; index < transport.range_count; ++index) {
        const auto& range = transport.ranges[index];
        if (range.frame_count == 0 || range.sample_offset != prior_end ||
            range.sample_offset > transport.frame_count ||
            range.frame_count > transport.frame_count - range.sample_offset ||
            !add_fits(range.timeline_sample_start.value, range.frame_count))
            return false;
        prior_end = range.sample_offset + range.frame_count;
    }
    return prior_end == transport.frame_count;
}

void render_track(const AudioTrackRendererProgram& track, const TransportRange& range,
                  audio::BufferView<float> output, bool absolute_only = false) noexcept {
    const auto range_start = range.timeline_sample_start.value;
    const auto range_end = range_start + static_cast<std::int64_t>(range.frame_count);
    const auto clips = track.clips();
    auto clip = std::lower_bound(clips.begin(), clips.end(), range_start,
                                 [](const AudioClipRendererProgram& candidate, std::int64_t value) {
                                     return candidate.timeline_end() <= value;
                                 });
    for (; clip != clips.end() && clip->timeline_start < range_end; ++clip) {
        if (absolute_only && clip->time_domain != AudioClipRendererProgram::TimeDomain::Absolute)
            continue;
        const auto media_end = clip->timeline_start +
                               static_cast<std::int64_t>(std::min(
                                   clip->timeline_frame_count, clip->renderable_timeline_frames));
        const auto overlap_start = std::max(range_start, clip->timeline_start);
        const auto overlap_end = std::min(range_end, media_end);
        if (overlap_end <= overlap_start)
            continue;
        const auto relative_start =
            static_cast<std::uint64_t>(overlap_start - clip->timeline_start);
        const auto output_start = static_cast<std::size_t>(range.sample_offset) +
                                  static_cast<std::size_t>(overlap_start - range_start);
        const auto count = static_cast<std::size_t>(overlap_end - overlap_start);
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
            auto destination = output.channel(channel);
            for (std::size_t frame = 0; frame < count; ++frame) {
                const auto relative = relative_start + frame;
                const auto source_position =
                    static_cast<long double>(relative) *
                    static_cast<long double>(clip->source_frames_per_timeline_frame);
                const auto source_offset = std::min(static_cast<std::uint64_t>(source_position),
                                                    clip->source_frame_count - 1u);
                const auto source_frame = clip->source_start + source_offset;
                const auto source_last = clip->source_start + clip->source_frame_count - 1u;
                const auto next_frame = std::min(source_frame + 1u, source_last);
                const auto fraction =
                    source_offset + 1u < clip->source_frame_count
                        ? static_cast<float>(source_position -
                                             static_cast<long double>(source_offset))
                        : 0.0f;
                destination[output_start + frame] +=
                    source_sample(*clip, channel, output.num_channels(), source_frame, next_frame,
                                  fraction, static_cast<double>(source_position)) *
                    envelope(*clip, relative);
            }
        }
    }
}

void render_host_beat_mapped_track(const AudioTrackRendererProgram& track,
                                   const TransportRange& range,
                                   const timebase::CompiledTempoMap& tempo_map,
                                   audio::BufferView<float> output) noexcept {
    render_track(track, range, output, true);
    const auto clips = track.clips();
    for (std::uint32_t output_frame = 0; output_frame < range.frame_count; ++output_frame) {
        const auto fraction =
            static_cast<long double>(output_frame) / static_cast<long double>(range.frame_count);
        const auto range_tick_start =
            range.has_precise_host_ticks
                ? static_cast<long double>(range.host_tick_start)
                : static_cast<long double>(range.timeline_tick_start.value);
        const auto range_tick_end = range.has_precise_host_ticks
                                        ? static_cast<long double>(range.host_tick_end)
                                        : static_cast<long double>(range.timeline_tick_end.value);
        const auto document_tick =
            range_tick_start + (range_tick_end - range_tick_start) * fraction;
        const auto document_position = tempo_map.fractional_ticks_to_samples(document_tick);
        auto clip = std::lower_bound(
            clips.begin(), clips.end(), document_position,
            [&tempo_map](const AudioClipRendererProgram& candidate, long double position) {
                const auto end =
                    candidate.time_domain == AudioClipRendererProgram::TimeDomain::Musical
                        ? tempo_map.fractional_ticks_to_samples(
                              static_cast<long double>(candidate.musical_tick_end.value))
                        : static_cast<long double>(candidate.timeline_end());
                return end <= position;
            });
        for (; clip != clips.end(); ++clip) {
            if (clip->time_domain != AudioClipRendererProgram::TimeDomain::Musical)
                continue;
            const auto clip_tick_start = static_cast<long double>(clip->musical_tick_start.value);
            const auto clip_tick_end = static_cast<long double>(clip->musical_tick_end.value);
            if (document_tick < clip_tick_start)
                break;
            if (!(document_tick < clip_tick_end))
                continue;
            const auto clip_start_position = tempo_map.fractional_ticks_to_samples(clip_tick_start);
            const auto relative = document_position - clip_start_position;
            if (relative < 0.0L ||
                relative >= static_cast<long double>(clip->renderable_timeline_frames))
                continue;
            const auto source_position =
                relative * static_cast<long double>(clip->source_frames_per_timeline_frame);
            const auto next_document_position =
                host_mapped_document_sample_at_output_offset(range, tempo_map, output_frame + 1u);
            const auto source_frames_per_output_frame = static_cast<double>(
                std::abs(next_document_position - document_position) *
                static_cast<long double>(clip->source_frames_per_timeline_frame));
            const auto source_offset =
                std::min(static_cast<std::uint64_t>(std::floor(source_position)),
                         clip->source_frame_count - 1u);
            const auto source_frame = clip->source_start + source_offset;
            const auto source_last = clip->source_start + clip->source_frame_count - 1u;
            const auto next_frame = std::min(source_frame + 1u, source_last);
            const auto fraction =
                source_offset + 1u < clip->source_frame_count
                    ? static_cast<float>(source_position - static_cast<long double>(source_offset))
                    : 0.0f;
            const auto destination_frame =
                static_cast<std::size_t>(range.sample_offset + output_frame);
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
                output.channel(channel)[destination_frame] +=
                    source_sample(*clip, channel, output.num_channels(), source_frame, next_frame,
                                  fraction, static_cast<double>(source_position),
                                  source_frames_per_output_frame) *
                    envelope(*clip, relative);
            }
        }
    }
}

} // namespace

runtime::Result<std::shared_ptr<const DecodedAudioAssetPool>, AudioRendererError>
DecodedAudioAssetPool::create(std::vector<DecodedAudioAsset> assets, AudioRendererLimits limits) {
    if (limits.max_channels == 0 || limits.max_asset_frames == 0)
        return fail<std::shared_ptr<const DecodedAudioAssetPool>>(
            AudioRendererErrorCode::CapacityExceeded);
    std::sort(assets.begin(), assets.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.id < rhs.id; });
    for (std::size_t index = 0; index < assets.size(); ++index) {
        const auto& asset = assets[index];
        if (!asset.id.valid())
            return fail<std::shared_ptr<const DecodedAudioAssetPool>>(
                AudioRendererErrorCode::InvalidIdentity, asset.id);
        if (index != 0 && assets[index - 1].id == asset.id)
            return fail<std::shared_ptr<const DecodedAudioAssetPool>>(
                AudioRendererErrorCode::DuplicateAsset, asset.id);
        if (!asset.audio || !valid_audio(*asset.audio, limits))
            return fail<std::shared_ptr<const DecodedAudioAssetPool>>(
                AudioRendererErrorCode::InvalidAsset, asset.id);
    }
    return runtime::Ok(
        std::shared_ptr<const DecodedAudioAssetPool>(new DecodedAudioAssetPool(std::move(assets))));
}

runtime::Result<DecodedAudioAsset, AudioRendererError>
DecodedAudioAssetPool::decode_wav(timeline::ItemId id, std::span<const std::uint8_t> bytes,
                                  audio::WavDecodeLimits decode_limits) {
    if (!id.valid())
        return fail<DecodedAudioAsset>(AudioRendererErrorCode::InvalidIdentity, id);
    auto decoded = audio::decode_wav(bytes, decode_limits);
    if (!decoded)
        return fail<DecodedAudioAsset>(AudioRendererErrorCode::InvalidAsset, id);
    return runtime::Ok(
        DecodedAudioAsset{id, std::make_shared<const audio::AudioFileData>(std::move(*decoded))});
}

const DecodedAudioAsset* DecodedAudioAssetPool::find(timeline::ItemId id) const noexcept {
    const auto found = std::lower_bound(
        assets_.begin(), assets_.end(), id,
        [](const DecodedAudioAsset& asset, timeline::ItemId wanted) { return asset.id < wanted; });
    return found != assets_.end() && found->id == id ? &*found : nullptr;
}

std::int64_t AudioClipRendererProgram::timeline_end() const noexcept {
    return timeline_start + static_cast<std::int64_t>(timeline_frame_count);
}

runtime::Result<AudioClipRendererProgram, AudioRendererError>
detail::compile_audio_clip_program_cached(const timeline::Clip& clip,
                                          const timeline::Project& project,
                                          const timebase::CompiledTempoMap& tempo_map,
                                          const DecodedAudioAssetPool& assets,
                                          const AudioRendererLimits& limits,
                                          AudioSampleRateConverterCache& cache) {
    const auto* media = std::get_if<timeline::MediaRef>(&clip.content());
    if (!media)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, clip.id());
    auto resolved = resolve_audio_media(clip.id(), *media, project, assets, limits);
    if (!resolved)
        return runtime::Err(resolved.error());
    if (resolved->decoded->audio->sample_rate > timebase::kMaximumCompiledSampleRate ||
        tempo_map.sample_rate().as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              clip.id(), media->asset_id);

    std::int64_t timeline_start = 0;
    std::uint64_t timeline_frames = 0;
    std::uint64_t fade_in_frames = 0;
    std::uint64_t fade_out_frames = 0;
    const auto source_rate = resolved->source_rate;
    const auto timeline_rate = tempo_map.sample_rate().normalized();
    auto converter =
        cache.impl_->get(source_rate, timeline_rate, clip.id(), media->asset_id, limits);
    if (!converter)
        return runtime::Err(converter.error());
    const auto renderable_frames =
        scale_duration_ceil(media->frame_count, source_rate, timeline_rate);
    if (!renderable_frames ||
        *renderable_frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              clip.id(), media->asset_id);
    const auto playback = clip.playback_properties();
    if (clip.time_anchor() == timeline::ClipTimeAnchor::Musical) {
        const auto start = tempo_map.ticks_to_samples(clip.start()).value;
        const auto end = tempo_map.ticks_to_samples(clip.end()).value;
        timeline_start = start;
        timeline_frames = positive_distance(start, end);
        const auto fade_in_tick =
            clip.start() +
            timebase::TickDuration{static_cast<std::int64_t>(playback.fade_in_duration)};
        const auto fade_out_tick =
            clip.end() -
            timebase::TickDuration{static_cast<std::int64_t>(playback.fade_out_duration)};
        fade_in_frames = positive_distance(start, tempo_map.ticks_to_samples(fade_in_tick).value);
        fade_out_frames = positive_distance(tempo_map.ticks_to_samples(fade_out_tick).value, end);
    } else {
        if (clip.absolute_sample_rate().as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
            return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                                  clip.id(), media->asset_id);
        const auto projected_start =
            scale_position(clip.absolute_start().value, clip.absolute_sample_rate(), timeline_rate);
        const auto projected_end =
            scale_position(clip.absolute_end().value, clip.absolute_sample_rate(), timeline_rate);
        const auto fade_in_position =
            clip.absolute_start().value + static_cast<std::int64_t>(playback.fade_in_duration);
        const auto fade_out_position =
            clip.absolute_end().value - static_cast<std::int64_t>(playback.fade_out_duration);
        const auto projected_fade_in_end =
            scale_position(fade_in_position, clip.absolute_sample_rate(), timeline_rate);
        const auto projected_fade_out_start =
            scale_position(fade_out_position, clip.absolute_sample_rate(), timeline_rate);
        if (!projected_start || !projected_end || !projected_fade_in_end ||
            !projected_fade_out_start || *projected_end < *projected_start ||
            *projected_fade_in_end < *projected_start || *projected_fade_out_start > *projected_end)
            return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                                  clip.id(), media->asset_id);
        timeline_start = *projected_start;
        timeline_frames = positive_distance(*projected_start, *projected_end);
        fade_in_frames = positive_distance(*projected_start, *projected_fade_in_end);
        fade_out_frames = positive_distance(*projected_fade_out_start, *projected_end);
    }
    if (timeline_frames == 0 || !add_fits(timeline_start, timeline_frames))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, clip.id(),
                                              media->asset_id);
    if (!std::isfinite(playback.gain_linear) || playback.gain_linear < 0.0f ||
        fade_in_frames > timeline_frames || fade_out_frames > timeline_frames)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidFade, clip.id());
    std::shared_ptr<const audio::PreparedVariableRateConversion> host_rate_converter;
    if (clip.time_anchor() == timeline::ClipTimeAnchor::Musical) {
        auto prepared =
            cache.impl_->get_host(resolved->decoded->audio, resolved->source_start,
                                  media->frame_count, clip.id(), media->asset_id, limits);
        if (!prepared)
            return runtime::Err(prepared.error());
        host_rate_converter = std::move(prepared).value();
    }
    return runtime::Ok(AudioClipRendererProgram{
        clip.id(), media->asset_id, resolved->decoded->audio, timeline_start, timeline_frames,
        resolved->source_start, media->frame_count, *renderable_frames,
        static_cast<double>(source_rate.as_long_double() / timeline_rate.as_long_double()),
        std::move(converter).value(), playback.gain_linear, fade_in_frames, fade_out_frames,
        AudioClipRendererProgram::SourceKind::ArrangementClip, 0,
        clip.time_anchor() == timeline::ClipTimeAnchor::Musical
            ? AudioClipRendererProgram::TimeDomain::Musical
            : AudioClipRendererProgram::TimeDomain::Absolute,
        std::move(host_rate_converter),
        clip.time_anchor() == timeline::ClipTimeAnchor::Musical ? clip.start()
                                                                : timebase::TickPosition{},
        clip.time_anchor() == timeline::ClipTimeAnchor::Musical ? clip.end()
                                                                : timebase::TickPosition{}});
}

runtime::Result<bool, AudioRendererError> detail::prepare_audio_clip_sample_rate_converters(
    const timeline::Clip& clip, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache) {
    const auto* media = std::get_if<timeline::MediaRef>(&clip.content());
    if (!media)
        return runtime::Ok(true);
    auto resolved = resolve_audio_media(clip.id(), *media, project, assets, limits);
    if (!resolved)
        return runtime::Err(resolved.error());
    if (resolved->decoded->audio->sample_rate > timebase::kMaximumCompiledSampleRate ||
        tempo_map.sample_rate().as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return runtime::Err(AudioRendererError{AudioRendererErrorCode::UnsupportedSampleRate,
                                               clip.id(), media->asset_id});
    auto fixed = cache.prepare(resolved->source_rate, tempo_map.sample_rate(), clip.id(),
                               media->asset_id, limits);
    if (!fixed || !*fixed)
        return fixed;
    if (clip.time_anchor() != timeline::ClipTimeAnchor::Musical)
        return runtime::Ok(true);
    return cache.prepare_host(resolved->decoded->audio, resolved->source_start, media->frame_count,
                              clip.id(), media->asset_id, limits);
}

runtime::Result<bool, AudioRendererError> detail::prepare_take_comp_segment_sample_rate_converter(
    const timeline::TakeLane& lane, std::size_t segment_index, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache) {
    const auto segments = lane.comp_segments();
    if (segment_index >= segments.size())
        return fail<bool>(AudioRendererErrorCode::InvalidTakeComp, lane.id());
    const auto* take = lane.find_take(segments[segment_index].take_id);
    if (!take)
        return runtime::Err(AudioRendererError{AudioRendererErrorCode::InvalidTakeComp, lane.id(),
                                               segments[segment_index].take_id});
    auto resolved = resolve_audio_media(take->id(), take->media(), project, assets, limits,
                                        AudioRendererErrorCode::InvalidTakeComp);
    if (!resolved)
        return runtime::Err(resolved.error());
    const auto source_rate = take->sample_rate().normalized();
    const auto timeline_rate = tempo_map.sample_rate().normalized();
    if (source_rate != resolved->source_rate ||
        source_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate) ||
        timeline_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return runtime::Err(AudioRendererError{AudioRendererErrorCode::UnsupportedSampleRate,
                                               take->id(), take->media().asset_id});
    return cache.prepare(source_rate, timeline_rate, take->id(), take->media().asset_id, limits);
}

runtime::Result<bool, AudioRendererError> detail::prepare_track_freeze_sample_rate_converter(
    const timeline::Track& track, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache) {
    if (!track.freeze())
        return fail<bool>(AudioRendererErrorCode::InvalidClipRange, track.id());
    auto resolved = resolve_audio_media(track.id(), track.freeze()->media, project, assets, limits);
    if (!resolved)
        return runtime::Err(resolved.error());
    const auto source_rate = track.freeze()->sample_rate.normalized();
    const auto timeline_rate = tempo_map.sample_rate().normalized();
    if (source_rate != resolved->source_rate ||
        source_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate) ||
        timeline_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return runtime::Err(AudioRendererError{AudioRendererErrorCode::UnsupportedSampleRate,
                                               track.id(), track.freeze()->media.asset_id});
    return cache.prepare(source_rate, timeline_rate, track.id(), track.freeze()->media.asset_id,
                         limits);
}

runtime::Result<AudioClipRendererProgram, AudioRendererError> compile_take_comp_segment_program(
    const timeline::TakeLane& lane, std::size_t segment_index, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits) {
    detail::AudioSampleRateConverterCache cache;
    return detail::compile_take_comp_segment_program_cached(lane, segment_index, project, tempo_map,
                                                            assets, limits, cache);
}

runtime::Result<AudioClipRendererProgram, AudioRendererError>
compile_audio_clip_program(const timeline::Clip& clip, const timeline::Project& project,
                           const timebase::CompiledTempoMap& tempo_map,
                           const DecodedAudioAssetPool& assets, const AudioRendererLimits& limits) {
    detail::AudioSampleRateConverterCache cache;
    return detail::compile_audio_clip_program_cached(clip, project, tempo_map, assets, limits,
                                                     cache);
}

runtime::Result<AudioClipRendererProgram, AudioRendererError>
detail::compile_take_comp_segment_program_cached(
    const timeline::TakeLane& lane, std::size_t segment_index, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache) {
    const auto segments = lane.comp_segments();
    if (segment_index >= segments.size() ||
        segment_index >= std::numeric_limits<std::uint32_t>::max())
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidTakeComp, lane.id());
    const auto& segment = segments[segment_index];
    const auto* take = lane.find_take(segment.take_id);
    if (!take)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidTakeComp, lane.id(),
                                              segment.take_id);
    auto resolved = resolve_audio_media(take->id(), take->media(), project, assets, limits,
                                        AudioRendererErrorCode::InvalidTakeComp);
    if (!resolved)
        return runtime::Err(resolved.error());
    const auto source_rate = take->sample_rate().normalized();
    const auto timeline_rate = tempo_map.sample_rate().normalized();
    if (source_rate != resolved->source_rate ||
        source_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate) ||
        timeline_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              take->id(), take->media().asset_id);
    auto converter =
        cache.impl_->get(source_rate, timeline_rate, take->id(), take->media().asset_id, limits);
    if (!converter)
        return runtime::Err(converter.error());
    const auto offset =
        static_cast<std::uint64_t>(segment.range.start.value - take->placement_start().value);
    if (offset > take->media().frame_count ||
        segment.range.sample_count > take->media().frame_count - offset)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidTakeComp, lane.id(),
                                              take->id());
    const auto end =
        segment.range.start.value + static_cast<std::int64_t>(segment.range.sample_count);
    const auto projected_start =
        scale_media_position(segment.range.start.value, source_rate, timeline_rate);
    const auto projected_end = scale_media_position(end, source_rate, timeline_rate);
    if (!projected_start || !projected_end || *projected_end <= *projected_start)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              take->id(), take->media().asset_id);
    const auto timeline_frames = positive_distance(*projected_start, *projected_end);
    if (!add_fits(*projected_start, timeline_frames))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidTakeComp, lane.id(),
                                              take->id());
    return runtime::Ok(AudioClipRendererProgram{
        take->id(),
        take->media().asset_id,
        resolved->decoded->audio,
        *projected_start,
        timeline_frames,
        resolved->source_start + offset,
        segment.range.sample_count,
        timeline_frames,
        static_cast<double>(source_rate.as_long_double() / timeline_rate.as_long_double()),
        std::move(converter).value(),
        1.0f,
        0,
        0,
        AudioClipRendererProgram::SourceKind::TakeCompSegment,
        static_cast<std::uint32_t>(segment_index + 1),
        AudioClipRendererProgram::TimeDomain::Absolute,
        nullptr,
        {},
        {},
    });
}

runtime::Result<AudioClipRendererProgram, AudioRendererError>
detail::compile_track_freeze_program_cached(const timeline::Track& track,
                                            const timeline::Project& project,
                                            const timebase::CompiledTempoMap& tempo_map,
                                            const DecodedAudioAssetPool& assets,
                                            const AudioRendererLimits& limits,
                                            AudioSampleRateConverterCache& cache) {
    if (!track.freeze())
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, track.id());
    const auto& freeze = *track.freeze();
    auto resolved = resolve_audio_media(track.id(), freeze.media, project, assets, limits);
    if (!resolved)
        return runtime::Err(resolved.error());
    const auto source_rate = freeze.sample_rate.normalized();
    const auto timeline_rate = tempo_map.sample_rate().normalized();
    if (source_rate != resolved->source_rate ||
        source_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate) ||
        timeline_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              track.id(), freeze.media.asset_id);
    auto converter =
        cache.impl_->get(source_rate, timeline_rate, track.id(), freeze.media.asset_id, limits);
    if (!converter)
        return runtime::Err(converter.error());
    if (freeze.media.frame_count >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, track.id(),
                                              freeze.media.asset_id);
    const auto signed_frames = static_cast<std::int64_t>(freeze.media.frame_count);
    if (freeze.placement_start.value > std::numeric_limits<std::int64_t>::max() - signed_frames)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, track.id(),
                                              freeze.media.asset_id);
    const auto end = freeze.placement_start.value + signed_frames;
    const auto projected_start =
        scale_media_position(freeze.placement_start.value, source_rate, timeline_rate);
    const auto projected_end = scale_media_position(end, source_rate, timeline_rate);
    if (!projected_start || !projected_end || *projected_end <= *projected_start)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              track.id(), freeze.media.asset_id);
    const auto timeline_frames = positive_distance(*projected_start, *projected_end);
    if (!add_fits(*projected_start, timeline_frames))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, track.id(),
                                              freeze.media.asset_id);
    return runtime::Ok(AudioClipRendererProgram{
        track.id(),
        freeze.media.asset_id,
        resolved->decoded->audio,
        *projected_start,
        timeline_frames,
        resolved->source_start,
        freeze.media.frame_count,
        timeline_frames,
        static_cast<double>(source_rate.as_long_double() / timeline_rate.as_long_double()),
        std::move(converter).value(),
        1.0f,
        0,
        0,
        AudioClipRendererProgram::SourceKind::FrozenTrack,
        0,
        AudioClipRendererProgram::TimeDomain::Absolute,
        nullptr,
        {},
        {},
    });
}

runtime::Result<AudioClipRendererProgram, AudioRendererError>
compile_track_freeze_program(const timeline::Track& track, const timeline::Project& project,
                             const timebase::CompiledTempoMap& tempo_map,
                             const DecodedAudioAssetPool& assets,
                             const AudioRendererLimits& limits) {
    detail::AudioSampleRateConverterCache cache;
    return detail::compile_track_freeze_program_cached(track, project, tempo_map, assets, limits,
                                                       cache);
}

runtime::Result<std::shared_ptr<const AudioTrackRendererProgram>, AudioRendererError>
link_audio_track_program(timeline::ItemId track_id, std::vector<AudioClipRendererProgram> clips,
                         const AudioRendererLimits& limits) {
    if (!track_id.valid())
        return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
            AudioRendererErrorCode::InvalidIdentity, track_id);
    if (clips.size() > limits.max_clips)
        return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
            AudioRendererErrorCode::CapacityExceeded, track_id, {}, clips.size(), limits.max_clips);
    using Key = std::tuple<AudioClipRendererProgram::SourceKind, std::uint64_t>;
    std::vector<Key> ids;
    ids.reserve(clips.size());
    for (const auto& clip : clips) {
        std::uint64_t source_identity = 0;
        switch (clip.source_kind) {
        case AudioClipRendererProgram::SourceKind::ArrangementClip:
            if (clip.source_ordinal != 0)
                return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
                    AudioRendererErrorCode::InvalidIdentity, clip.id);
            source_identity = clip.id.value;
            break;
        case AudioClipRendererProgram::SourceKind::TakeCompSegment:
            if (clip.source_ordinal == 0)
                return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
                    AudioRendererErrorCode::InvalidIdentity, clip.id);
            source_identity = clip.source_ordinal;
            break;
        case AudioClipRendererProgram::SourceKind::FrozenTrack:
            if (clip.source_ordinal != 0 || clip.id != track_id)
                return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
                    AudioRendererErrorCode::InvalidIdentity, clip.id);
            source_identity = clip.id.value;
            break;
        default:
            return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
                AudioRendererErrorCode::InvalidIdentity, clip.id);
        }
        ids.emplace_back(clip.source_kind, source_identity);
    }
    std::sort(ids.begin(), ids.end());
    const auto duplicate = std::adjacent_find(ids.begin(), ids.end());
    if (duplicate != ids.end())
        return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
            AudioRendererErrorCode::InvalidIdentity,
            std::get<0>(*duplicate) == AudioClipRendererProgram::SourceKind::ArrangementClip
                ? timeline::ItemId{std::get<1>(*duplicate)}
                : track_id);
    std::sort(clips.begin(), clips.end(), [](const auto& lhs, const auto& rhs) {
        return std::tuple(lhs.timeline_start, lhs.source_kind, lhs.id.value, lhs.source_ordinal) <
               std::tuple(rhs.timeline_start, rhs.source_kind, rhs.id.value, rhs.source_ordinal);
    });
    return runtime::Ok(std::shared_ptr<const AudioTrackRendererProgram>(
        new AudioTrackRendererProgram(track_id, std::move(clips))));
}

AudioRenderStatus ArrangementAudioRenderer::process(const PlaybackProgram& program,
                                                    const TransportSnapshot& transport,
                                                    audio::BufferView<float> output,
                                                    AudioRendererLimits limits) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    const auto& compiled_limits = program.audio_limits();
    limits.max_channels = std::min(limits.max_channels, compiled_limits.max_channels);
    limits.max_block_frames = std::min(limits.max_block_frames, compiled_limits.max_block_frames);
    limits.max_asset_frames = std::min(limits.max_asset_frames, compiled_limits.max_asset_frames);
    limits.max_tracks = std::min(limits.max_tracks, compiled_limits.max_tracks);
    limits.max_clips = std::min(limits.max_clips, compiled_limits.max_clips);
    if (output.empty())
        return AudioRenderStatus::InvalidOutput;
    if (output.num_channels() > limits.max_channels ||
        output.num_samples() > limits.max_block_frames)
        return AudioRenderStatus::CapacityExceeded;
    output.clear();
    if (!valid_transport(transport, output.num_samples(), program.tempo_map(), limits))
        return AudioRenderStatus::InvalidTransport;
    if (!transport.is_playing)
        return AudioRenderStatus::Silent;

    bool rendered = false;
    if (program.tracks().size() > limits.max_tracks)
        return AudioRenderStatus::CapacityExceeded;

    std::uint64_t total_clips = 0;
    for (const auto& track : program.tracks()) {
        if (!track->audio_program())
            continue;
        if (track->audio_program()->id() != track->id())
            return AudioRenderStatus::InvalidProgram;
        const auto clip_count = static_cast<std::uint64_t>(track->audio_program()->clips().size());
        if (clip_count > limits.max_clips - total_clips)
            return AudioRenderStatus::CapacityExceeded;
        total_clips += clip_count;
    }

    for (const auto& track : program.tracks()) {
        const auto provider = track->provider();
        if (provider.selected != ProviderKind::Arrangement ||
            !provider.available(ProviderKind::Arrangement) || !track->audio_program())
            continue;
        for (std::uint8_t index = 0; index < transport.range_count; ++index) {
            const auto& range = transport.ranges[index];
            if (range.host_beat_mapping) {
                render_host_beat_mapped_track(*track->audio_program(), range, program.tempo_map(),
                                              output);
            } else {
                render_track(*track->audio_program(), range, output);
            }
        }
        rendered = rendered || !track->audio_program()->clips().empty();
    }
    return rendered ? AudioRenderStatus::Rendered : AudioRenderStatus::Silent;
}

AudioRenderStatus ArrangementAudioTrackRenderer::process(const PlaybackProgramBlock& block,
                                                         const TransportSnapshot& transport,
                                                         audio::BufferView<float> output,
                                                         AudioRendererLimits limits) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    if (output.empty())
        return AudioRenderStatus::InvalidOutput;
    // Reject caller-bounded shapes before touching memory. The compiled program
    // can only narrow these bounds; that second check below is also performed
    // before clear so CapacityExceeded always leaves the caller's sentinel data
    // intact.
    if (output.num_channels() > limits.max_channels ||
        output.num_samples() > limits.max_block_frames)
        return AudioRenderStatus::CapacityExceeded;

    const auto view = shell_.begin_block(block);
    if (!view.program) {
        if (view.adoption == ShellAdoptionResult::Rejected)
            return AudioRenderStatus::InvalidProgram;
        output.clear();
        return AudioRenderStatus::Silent;
    }
    const auto& program = *block.program();
    const auto& compiled_limits = program.audio_limits();
    limits.max_channels = std::min(limits.max_channels, compiled_limits.max_channels);
    limits.max_block_frames = std::min(limits.max_block_frames, compiled_limits.max_block_frames);
    limits.max_asset_frames = std::min(limits.max_asset_frames, compiled_limits.max_asset_frames);
    limits.max_tracks = std::min(limits.max_tracks, compiled_limits.max_tracks);
    limits.max_clips = std::min(limits.max_clips, compiled_limits.max_clips);
    if (output.num_channels() > limits.max_channels ||
        output.num_samples() > limits.max_block_frames)
        return AudioRenderStatus::CapacityExceeded;
    const auto provider = view.program->provider();
    const auto* audio_program = view.program->audio_program();
    const bool arrangement_selected = provider.selected == ProviderKind::Arrangement &&
                                      provider.available(ProviderKind::Arrangement);
    if (transport.is_playing && arrangement_selected && audio_program != nullptr) {
        if (audio_program->id() != view.program->id())
            return AudioRenderStatus::InvalidProgram;
        if (audio_program->clips().size() > limits.max_clips)
            return AudioRenderStatus::CapacityExceeded;
    }
    if (!valid_transport(transport, output.num_samples(), program.tempo_map(), limits)) {
        output.clear();
        return AudioRenderStatus::InvalidTransport;
    }
    output.clear();

    AudioRenderStatus status = AudioRenderStatus::Silent;
    if (transport.is_playing && arrangement_selected && audio_program != nullptr) {
        for (std::uint8_t index = 0; index < transport.range_count; ++index) {
            const auto& range = transport.ranges[index];
            if (range.host_beat_mapping) {
                render_host_beat_mapped_track(*audio_program, range, program.tempo_map(), output);
            } else {
                render_track(*audio_program, range, output);
            }
        }
        if (!audio_program->clips().empty())
            status = AudioRenderStatus::Rendered;
    }

    RendererCarryState carry = shell_.state_snapshot();
    carry.key = shell_.active_key();
    carry.active_provider = provider.selected;
    carry.valid = true;
    if (transport.range_count != 0) {
        const auto& last = transport.ranges[transport.range_count - 1];
        carry.source_sample =
            last.host_beat_mapping
                ? program.tempo_map().ticks_to_samples(last.timeline_tick_end)
                : timebase::SamplePosition{last.timeline_sample_start.value +
                                           static_cast<std::int64_t>(last.frame_count)};
        carry.timeline_tick = last.timeline_tick_end;
        carry.loop_iteration += transport.range_count > 1 ? 1u : 0u;
    }
    if (!shell_.end_block(carry))
        return AudioRenderStatus::InvalidProgram;
    return status;
}

} // namespace pulp::playback
