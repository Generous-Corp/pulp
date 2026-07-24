#include "audio_renderer_internal.hpp"

#include <pulp/audio/sample_rate_conversion.hpp>

#include <algorithm>
#include <limits>
#include <memory>

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

    struct EntryNode {
        Entry value;
        std::unique_ptr<EntryNode> next;
    };

    struct HostRateEntryNode {
        HostRateEntry value;
        std::unique_ptr<HostRateEntryNode> next;
    };

    ~Impl() {
        while (entries)
            entries = std::move(entries->next);
        while (host_rate_entries)
            host_rate_entries = std::move(host_rate_entries->next);
    }

    std::size_t converter_count() const noexcept {
        return entry_count + host_rate_entry_count + (fixed_rate_builder ? 1u : 0u) +
               (host_rate_builder ? 1u : 0u);
    }

    Entry* find_entry(timebase::RationalRate source, timebase::RationalRate target) noexcept {
        for (auto* node = entries.get(); node != nullptr; node = node->next.get()) {
            if (node->value.source == source && node->value.target == target)
                return &node->value;
        }
        return nullptr;
    }

    HostRateEntry* find_host(const std::shared_ptr<const audio::AudioFileData>& source,
                             std::uint64_t source_start, std::uint64_t source_frames) noexcept {
        for (auto* node = host_rate_entries.get(); node != nullptr; node = node->next.get()) {
            if (node->value.source == source && node->value.source_start == source_start &&
                node->value.source_frames == source_frames)
                return &node->value;
        }
        return nullptr;
    }

    static std::uint64_t saturating_add(std::uint64_t lhs, std::uint64_t rhs) noexcept {
        return rhs <= std::numeric_limits<std::uint64_t>::max() - lhs
                   ? lhs + rhs
                   : std::numeric_limits<std::uint64_t>::max();
    }

    static std::uint64_t node_storage_bytes(std::size_t count, std::size_t node_size) noexcept {
        const auto charged_size = saturating_add(node_size, audio::kConverterAllocationOverhead);
        if (charged_size != 0 && count > std::numeric_limits<std::uint64_t>::max() / charged_size)
            return std::numeric_limits<std::uint64_t>::max();
        return count * charged_size;
    }

    std::uint64_t cache_storage_bytes(std::size_t fixed_count,
                                      std::size_t host_count) const noexcept {
        return saturating_add(node_storage_bytes(fixed_count, sizeof(EntryNode)),
                              node_storage_bytes(host_count, sizeof(HostRateEntryNode)));
    }

    std::uint64_t required_bytes_with_counts(std::uint64_t additional, std::size_t fixed_count,
                                             std::size_t host_count) const noexcept {
        const auto storage = cache_storage_bytes(fixed_count, host_count);
        return saturating_add(saturating_add(converter_bytes, storage), additional);
    }

    void add_entry(Entry value) {
        auto node = std::make_unique<EntryNode>();
        node->value = std::move(value);
        node->next = std::move(entries);
        entries = std::move(node);
        ++entry_count;
    }

    void add_host_entry(HostRateEntry value) {
        auto node = std::make_unique<HostRateEntryNode>();
        node->value = std::move(value);
        node->next = std::move(host_rate_entries);
        host_rate_entries = std::move(node);
        ++host_rate_entry_count;
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
        return runtime::Ok(find_entry(source, target)->converter);
    }

    runtime::Result<bool, AudioRendererError>
    prepare(timebase::RationalRate source, timebase::RationalRate target, timeline::ItemId item,
            timeline::ItemId related_item, const AudioRendererLimits& limits) {
        source = source.normalized();
        target = target.normalized();
        if (source == target)
            return runtime::Ok(true);
        if (find_entry(source, target) != nullptr)
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
            const auto required = required_bytes_with_counts(estimated_bytes, entry_count + 1u,
                                                             host_rate_entry_count);
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
        const auto required =
            required_bytes_with_counts(actual_bytes, entry_count + 1u, host_rate_entry_count);
        if (required > limits.max_sample_rate_converter_bytes)
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::CapacityExceeded,
                item,
                related_item,
                required,
                limits.max_sample_rate_converter_bytes,
            });
        add_entry({source, target, std::move(converter)});
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
        if (const auto* found = find_entry(source, target))
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
        const auto required = required_bytes_with_counts(converter->prepared_bytes(),
                                                         entry_count + 1u, host_rate_entry_count);
        if (required > limits.max_sample_rate_converter_bytes) {
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::CapacityExceeded,
                item,
                related_item,
                required,
                limits.max_sample_rate_converter_bytes,
            });
        }
        add_entry({source, target, converter});
        converter_bytes += converter->prepared_bytes();
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
        if (find_host(source, source_start, source_frames) != nullptr)
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
            const auto required =
                bytes ? required_bytes_with_counts(*bytes, entry_count, host_rate_entry_count + 1u)
                      : std::numeric_limits<std::uint64_t>::max();
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
        const auto required =
            required_bytes_with_counts(actual_bytes, entry_count, host_rate_entry_count + 1u);
        if (required > limits.max_sample_rate_converter_bytes)
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::CapacityExceeded,
                item,
                related_item,
                required,
                limits.max_sample_rate_converter_bytes,
            });
        add_host_entry(
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
        if (found != nullptr)
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
        const auto required = required_bytes_with_counts(converter->prepared_bytes(), entry_count,
                                                         host_rate_entry_count + 1u);
        if (required > limits.max_sample_rate_converter_bytes) {
            return runtime::Err(AudioRendererError{
                AudioRendererErrorCode::CapacityExceeded,
                item,
                related_item,
                required,
                limits.max_sample_rate_converter_bytes,
            });
        }
        add_host_entry({std::move(source), source_start, source_frames, converter});
        converter_bytes += converter->prepared_bytes();
        return runtime::Ok(std::move(converter));
    }

    std::unique_ptr<EntryNode> entries;
    std::unique_ptr<HostRateEntryNode> host_rate_entries;
    std::size_t entry_count = 0;
    std::size_t host_rate_entry_count = 0;
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

runtime::Result<std::shared_ptr<const audio::PreparedSampleRateConversion>, AudioRendererError>
detail::AudioSampleRateConverterCache::get(timebase::RationalRate source,
                                           timebase::RationalRate target, timeline::ItemId item,
                                           timeline::ItemId related_item,
                                           const AudioRendererLimits& limits) {
    return impl_->get(source, target, item, related_item, limits);
}

runtime::Result<std::shared_ptr<const audio::PreparedVariableRateConversion>, AudioRendererError>
detail::AudioSampleRateConverterCache::get_host(std::shared_ptr<const audio::AudioFileData> source,
                                                std::uint64_t source_start,
                                                std::uint64_t source_frames, timeline::ItemId item,
                                                timeline::ItemId related_item,
                                                const AudioRendererLimits& limits) {
    return impl_->get_host(std::move(source), source_start, source_frames, item, related_item,
                           limits);
}

runtime::Result<bool, AudioRendererError> detail::AudioSampleRateConverterCache::prepare_host(
    std::shared_ptr<const audio::AudioFileData> source, std::uint64_t source_start,
    std::uint64_t source_frames, timeline::ItemId item, timeline::ItemId related_item,
    const AudioRendererLimits& limits) {
    return impl_->prepare_host(std::move(source), source_start, source_frames, item, related_item,
                               limits);
}

} // namespace pulp::playback
