#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_preload_contract.hpp>
#include <pulp/audio/sample_stream_service.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

namespace pulp::audio {

struct SampleAssetToken {
    std::uint64_t asset_id = 0;
    std::uint64_t asset_generation = 0;
};

struct SampleAssetConfig {
    SampleAssetToken asset{};
    SampleStreamSourceToken source{};
    std::uint32_t channels = 0;
    std::uint64_t total_frames = 0;
    double sample_rate = 0.0;
    std::uint64_t preload_frames = 0;
    // Required for a streamed tail. The loader supplies the sampler's actual
    // prepared host/rate/block/interpolation limits, not source-authored metadata.
    std::optional<SamplePreloadContract> preload_contract;
    std::optional<SampleStreamCacheSourceView> stream_source;
};

class SampleAsset;

class SampleAssetRegistrationProof {
public:
    SampleAssetRegistrationProof() = default;

    bool matches(SampleAssetToken asset,
                 SampleStreamSourceToken source,
                 const float* const* preload_channels,
                 std::uint32_t channels,
                 std::uint64_t total_frames,
                 double sample_rate,
                 std::uint64_t preload_frames,
                 bool has_preload_contract,
                 const SamplePreloadContract& preload_contract,
                 bool has_stream_source) const noexcept {
        return asset_.asset_id == asset.asset_id &&
               asset_.asset_generation == asset.asset_generation &&
               source_.source_id == source.source_id &&
               source_.source_generation == source.source_generation &&
               preload_channels_ == preload_channels && channels_ == channels &&
               total_frames_ == total_frames && sample_rate_ == sample_rate &&
               preload_frames_ == preload_frames &&
               has_preload_contract_ == has_preload_contract &&
               same_contract(preload_contract_, preload_contract) &&
               has_stream_source_ == has_stream_source;
    }

private:
    friend class SampleAsset;

    SampleAssetRegistrationProof(const SampleAssetConfig& config,
                                 const float* const* preload_channels) noexcept
        : asset_(config.asset),
          source_(config.source),
          preload_channels_(preload_channels),
          channels_(config.channels),
          total_frames_(config.total_frames),
          sample_rate_(config.sample_rate),
          preload_frames_(config.preload_frames),
          has_preload_contract_(config.preload_contract.has_value()),
          preload_contract_(config.preload_contract.value_or(SamplePreloadContract{})),
          has_stream_source_(config.stream_source.has_value()) {}

    static bool same_contract(const SamplePreloadContract& left,
                              const SamplePreloadContract& right) noexcept {
        return left.source_sample_rate == right.source_sample_rate &&
               left.host_sample_rate == right.host_sample_rate &&
               left.maximum_playback_ratio == right.maximum_playback_ratio &&
               left.certified_io_latency_seconds == right.certified_io_latency_seconds &&
               left.scheduler_margin_seconds == right.scheduler_margin_seconds &&
               left.decoder_latency_seconds == right.decoder_latency_seconds &&
               left.maximum_host_block_frames == right.maximum_host_block_frames &&
               left.interpolation_guard_frames == right.interpolation_guard_frames &&
               left.loop_prefetch_guard_frames == right.loop_prefetch_guard_frames &&
               left.configured_preload_frames == right.configured_preload_frames;
    }

    SampleAssetToken asset_{};
    SampleStreamSourceToken source_{};
    const float* const* preload_channels_ = nullptr;
    std::uint32_t channels_ = 0;
    std::uint64_t total_frames_ = 0;
    double sample_rate_ = 0.0;
    std::uint64_t preload_frames_ = 0;
    bool has_preload_contract_ = false;
    SamplePreloadContract preload_contract_{};
    bool has_stream_source_ = false;
};

static_assert(std::is_trivially_copyable_v<SampleAssetRegistrationProof>);
static_assert(std::is_standard_layout_v<SampleAssetRegistrationProof>);

/// Immutable audio-thread snapshot borrowed from one SampleAsset owner.
/// The owner and its stream source must outlive every copy of this view.
struct SampleAssetView {
    SampleAssetToken asset{};
    SampleStreamSourceToken source{};
    const float* const* preload_channels = nullptr;
    std::uint32_t channels = 0;
    std::uint64_t total_frames = 0;
    double sample_rate = 0.0;
    std::uint64_t preload_frames = 0;
    bool has_preload_contract = false;
    SamplePreloadContract preload_contract{};
    bool has_stream_source = false;
    SampleStreamCacheSourceView stream_source{};
    SampleAssetRegistrationProof registration{};

    bool valid() const noexcept {
        const bool base_valid = asset.asset_id != 0 && asset.asset_generation != 0 &&
                                source.source_id != 0 && source.source_generation != 0 &&
                                preload_channels != nullptr && channels != 0 &&
                                total_frames != 0 && sample_rate > 0.0 &&
                                std::isfinite(sample_rate) &&
                                preload_frames != 0 && preload_frames <= total_frames &&
                                (preload_frames == total_frames || has_stream_source);
        if (!base_valid ||
            !registration.matches(asset,
                                  source,
                                  preload_channels,
                                  channels,
                                  total_frames,
                                  sample_rate,
                                  preload_frames,
                                  has_preload_contract,
                                  preload_contract,
                                  has_stream_source)) {
            return false;
        }
        if (!has_stream_source) return true;
        if (preload_frames < total_frames &&
            (!has_preload_contract ||
             preload_contract.source_sample_rate != sample_rate ||
             preload_contract.configured_preload_frames != preload_frames)) {
            return false;
        }
        return stream_source.valid() &&
               stream_source.token.source_id == source.source_id &&
               stream_source.token.source_generation == source.source_generation &&
               stream_source.total_frames == total_frames;
    }

    bool fully_resident() const noexcept {
        return valid() && preload_frames == total_frames;
    }

    bool preload_contains(std::uint64_t frame) const noexcept {
        return valid() && frame < preload_frames;
    }

    const float* preload_channel_data(std::uint32_t channel) const noexcept {
        if (!valid() || channel >= channels) return nullptr;
        return preload_channels[channel];
    }
};

static_assert(std::is_trivially_copyable_v<SampleAssetView>);
static_assert(std::is_standard_layout_v<SampleAssetView>);

/// Off-audio-thread owner for immutable sampler metadata and resident preload.
/// prepare() copies the preload and fixes its channel pointers. Destructive
/// prepare/release is only valid after all borrowed SampleAssetView readers are
/// quiescent.
class SampleAsset {
public:
    SampleAsset() = default;
    SampleAsset(const SampleAsset&) = delete;
    SampleAsset& operator=(const SampleAsset&) = delete;
    SampleAsset(SampleAsset&&) = delete;
    SampleAsset& operator=(SampleAsset&&) = delete;

    template<typename SampleType>
    bool prepare(const SampleAssetConfig& config, BufferView<SampleType> preload) {
        static_assert(std::is_same_v<std::remove_const_t<SampleType>, float>);
        release();
        if (!config_valid(config, preload)) return false;

        try {
            preload_.resize(config.channels, static_cast<std::size_t>(config.preload_frames));
            preload_channel_ptrs_.resize(config.channels);
            for (std::uint32_t channel = 0; channel < config.channels; ++channel) {
                std::copy_n(preload.channel_ptr(channel),
                            static_cast<std::size_t>(config.preload_frames),
                            preload_.channel(channel).data());
                preload_channel_ptrs_[channel] = preload_.channel(channel).data();
            }
        } catch (...) {
            release();
            return false;
        }

        view_ = {
            .asset = config.asset,
            .source = config.source,
            .preload_channels = preload_channel_ptrs_.data(),
            .channels = config.channels,
            .total_frames = config.total_frames,
            .sample_rate = config.sample_rate,
            .preload_frames = config.preload_frames,
            .has_preload_contract = config.preload_contract.has_value(),
            .preload_contract = config.preload_contract.value_or(SamplePreloadContract{}),
            .has_stream_source = config.stream_source.has_value(),
            .stream_source = config.stream_source.value_or(SampleStreamCacheSourceView{}),
            .registration = SampleAssetRegistrationProof(
                config, preload_channel_ptrs_.data()),
        };
        return true;
    }

    void release() noexcept {
        view_ = {};
        std::vector<const float*>().swap(preload_channel_ptrs_);
        preload_ = {};
    }

    bool prepared() const noexcept { return view_.valid(); }
    SampleAssetView view() const noexcept { return view_; }

private:
    template<typename SampleType>
    static bool config_valid(const SampleAssetConfig& config,
                             BufferView<SampleType> preload) noexcept {
        if (config.asset.asset_id == 0 || config.asset.asset_generation == 0 ||
            config.source.source_id == 0 || config.source.source_generation == 0 ||
            config.channels == 0 || config.total_frames == 0 ||
            !(config.sample_rate > 0.0) || !std::isfinite(config.sample_rate) ||
            config.preload_frames == 0 || config.preload_frames > config.total_frames ||
            config.preload_frames > std::numeric_limits<std::size_t>::max() ||
            config.preload_frames >
                std::numeric_limits<std::size_t>::max() / config.channels ||
            preload.num_channels() != config.channels ||
            config.preload_frames > preload.num_samples()) {
            return false;
        }
        for (std::uint32_t channel = 0; channel < config.channels; ++channel) {
            if (preload.channel_ptr(channel) == nullptr) return false;
        }

        if (!config.stream_source) return config.preload_frames == config.total_frames;
        if (config.preload_frames < config.total_frames) {
            if (!config.preload_contract) return false;
            const auto& contract = *config.preload_contract;
            if (contract.source_sample_rate != config.sample_rate ||
                contract.configured_preload_frames != config.preload_frames ||
                contract.maximum_host_block_frames == 0 ||
                contract.interpolation_guard_frames == 0) {
                return false;
            }
            const auto evaluated = evaluate_sample_preload_contract(contract);
            if (!evaluated.valid() || !evaluated.sufficient) return false;
        }
        const auto& stream = *config.stream_source;
        return stream.valid() && stream.token.source_id == config.source.source_id &&
               stream.token.source_generation == config.source.source_generation &&
               stream.total_frames == config.total_frames && stream.window->prepared() &&
               stream.window->channels() == config.channels &&
               stream.window->page_frames() == stream.page_frames;
    }

    Buffer<float> preload_;
    std::vector<const float*> preload_channel_ptrs_;
    SampleAssetView view_{};
};

}  // namespace pulp::audio
